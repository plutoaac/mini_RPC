#include "client/rpc_client_pool.h"

#include <algorithm>
#include <string>

#include "client/load_balancer.h"
#include "common/log.h"

namespace rpc::client {

// ============================================================================
// 内部 Channel 结构
// ============================================================================

struct RpcClientPool::Channel {
  std::string host;
  std::uint16_t port;
  std::unique_ptr<RpcClient> client;

  // 运行时统计（原子，保证多线程安全）
  std::atomic<std::size_t> success_count{0};
  std::atomic<std::size_t> fail_count{0};
  std::atomic<std::size_t> select_count{0};
  std::atomic<std::size_t> consecutive_failures{0};
  std::atomic<bool> healthy{true};

  Channel(std::string h, std::uint16_t p, const RpcClientOptions& options)
      : host(std::move(h)), port(p), client(std::make_unique<RpcClient>(host, port, options)) {}

  [[nodiscard]] std::string EndpointString() const {
    return host + ":" + std::to_string(port);
  }
};

// ============================================================================
// 构造与析构
// ============================================================================

RpcClientPool::RpcClientPool(std::vector<Endpoint> endpoints,
                             RpcClientPoolOptions options)
    : options_(std::move(options)) {
  if (endpoints.empty()) {
    common::LogError("RpcClientPool created with empty endpoints");
    return;
  }

  channels_.reserve(endpoints.size());
  for (auto& ep : endpoints) {
    channels_.push_back(
        std::make_unique<Channel>(std::move(ep.host), ep.port, options_.client_options));
  }

  switch (options_.strategy) {
    case LoadBalanceStrategy::kLeastInflight:
      balancer_ = std::make_unique<LeastInflightBalancer>();
      break;
    case LoadBalanceStrategy::kRoundRobin:
    default:
      balancer_ = std::make_unique<RoundRobinBalancer>();
      break;
  }
}

RpcClientPool::~RpcClientPool() = default;

// ============================================================================
// 预热
// ============================================================================

bool RpcClientPool::Warmup() {
  std::size_t connected = 0;
  for (auto& ch : channels_) {
    if (ch->client->Connect()) {
      ++connected;
    } else {
      common::LogWarn("RpcClientPool warmup failed for " + ch->EndpointString());
    }
  }
  return connected > 0;
}

// ============================================================================
// 节点选择
// ============================================================================

RpcClientPool::Channel* RpcClientPool::SelectChannel() {
  // 1. 收集 healthy 候选节点
  std::vector<Channel*> healthy_candidates;
  healthy_candidates.reserve(channels_.size());

  for (auto& ch : channels_) {
    if (ch->healthy.load(std::memory_order_acquire)) {
      healthy_candidates.push_back(ch.get());
    }
  }

  // 2. 若全部不健康，做一次尽力而为：返回第一个 channel（让调用方自行失败）
  if (healthy_candidates.empty()) {
    if (!channels_.empty()) {
      common::LogWarn("RpcClientPool: all channels unhealthy, falling back");
      return channels_.front().get();
    }
    return nullptr;
  }

  // 3. 构建 RpcClient* 候选列表供 balancer 使用
  std::vector<RpcClient*> client_candidates;
  client_candidates.reserve(healthy_candidates.size());
  for (auto* ch : healthy_candidates) {
    client_candidates.push_back(ch->client.get());
  }

  const std::size_t selected_index = balancer_->Select(client_candidates);
  Channel* selected = healthy_candidates[selected_index];
  selected->select_count.fetch_add(1, std::memory_order_relaxed);
  return selected;
}

// ============================================================================
// 健康状态更新
// ============================================================================

void RpcClientPool::UpdateHealth(Channel* channel, bool success) {
  if (success) {
    channel->success_count.fetch_add(1, std::memory_order_relaxed);
    // 成功则清零连续失败计数
    channel->consecutive_failures.store(0, std::memory_order_release);
    channel->healthy.store(true, std::memory_order_release);
  } else {
    channel->fail_count.fetch_add(1, std::memory_order_relaxed);
    const std::size_t failures =
        channel->consecutive_failures.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (failures >= options_.max_consecutive_failures) {
      common::LogWarn("RpcClientPool: marking " + channel->EndpointString() +
                      " unhealthy after " + std::to_string(failures) +
                      " consecutive failures");
      channel->healthy.store(false, std::memory_order_release);
    }
  }
}

bool RpcClientPool::IsConnectionError(const RpcCallResult& result) {
  if (result.ok()) {
    return false;
  }
  // 简单的启发式判断：连接类错误通常包含这些关键词
  const std::string& msg = result.status.message;
  return msg.find("connect failed") != std::string::npos ||
         msg.find("send request failed") != std::string::npos ||
         msg.find("connection closed") != std::string::npos ||
         msg.find("connection inactive") != std::string::npos ||
         msg.find("dispatcher stopped") != std::string::npos ||
         msg.find("call timeout") != std::string::npos;
}

// ============================================================================
// 同步 Call（支持重试）
// ============================================================================

RpcCallResult RpcClientPool::Call(std::string_view service_name,
                                  std::string_view method_name,
                                  std::string_view request_payload) {
  if (channels_.empty()) {
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "no endpoints configured"},
        {}};
  }

  // 记录本轮已经尝试过的 channel，避免死循环重试同一个节点
  // 由于 SelectChannel 依赖 balancer 状态，多次调用可能选到不同节点
  const std::size_t max_attempts = channels_.size();
  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    Channel* ch = SelectChannel();
    if (!ch) {
      return RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "no channel available"},
          {}};
    }

    auto result = ch->client->Call(service_name, method_name, request_payload);

    if (result.ok()) {
      UpdateHealth(ch, true);
      return result;
    }

    // 失败处理：如果是连接类错误，标记节点不健康并尝试下一个
    if (IsConnectionError(result)) {
      UpdateHealth(ch, false);
      common::LogWarn("RpcClientPool: call failed on " + ch->EndpointString() +
                      ", retrying... attempt=" + std::to_string(attempt + 1));
      continue;
    }

    // 业务错误（如 MethodNotFound）不重试，直接返回
    UpdateHealth(ch, true);  // 业务错误不算连接失败，但计一次 fail_count
    return result;
  }

  return RpcCallResult{
      {common::make_error_code(common::ErrorCode::kInternalError),
       "all channels failed after " + std::to_string(max_attempts) + " attempts"},
      {}};
}

// ============================================================================
// 异步 CallAsync（不重试）
// ============================================================================

std::future<RpcCallResult> RpcClientPool::CallAsync(
    std::string_view service_name, std::string_view method_name,
    std::string_view request_payload) {
  Channel* ch = SelectChannel();
  if (!ch) {
    std::promise<RpcCallResult> promise;
    auto future = promise.get_future();
    promise.set_value(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "no channel available"},
        {}});
    return future;
  }

  ch->select_count.fetch_add(1, std::memory_order_relaxed);
  return ch->client->CallAsync(service_name, method_name, request_payload);
}

// ============================================================================
// 协程 CallCo（不重试）
// ============================================================================

rpc::coroutine::Task<RpcCallResult> RpcClientPool::CallCo(
    std::string_view service_name, std::string_view method_name,
    std::string_view request_payload) {
  Channel* ch = SelectChannel();
  if (!ch) {
    co_return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "no channel available"},
        {}};
  }

  ch->select_count.fetch_add(1, std::memory_order_relaxed);
  co_return co_await ch->client->CallCo(service_name, method_name,
                                        request_payload);
}

// ============================================================================
// 统计
// ============================================================================

std::vector<RpcClientPool::EndpointStats> RpcClientPool::GetStats() const {
  std::vector<EndpointStats> stats;
  stats.reserve(channels_.size());

  for (const auto& ch : channels_) {
    EndpointStats s;
    s.endpoint = ch->EndpointString();
    s.inflight = ch->client->GetInflightCount();
    s.success_count = ch->success_count.load(std::memory_order_relaxed);
    s.fail_count = ch->fail_count.load(std::memory_order_relaxed);
    s.select_count = ch->select_count.load(std::memory_order_relaxed);
    s.healthy = ch->healthy.load(std::memory_order_acquire);
    s.connected = ch->client->IsConnected();
    stats.push_back(std::move(s));
  }

  return stats;
}

const char* RpcClientPool::StrategyName() const {
  return balancer_ ? balancer_->Name() : "none";
}

}  // namespace rpc::client
