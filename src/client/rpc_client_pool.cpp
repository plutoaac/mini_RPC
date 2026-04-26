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
      // 预热成功：清零失败计数并标记健康
      ch->consecutive_failures.store(0, std::memory_order_release);
      ch->healthy.store(true, std::memory_order_release);
    } else {
      common::LogWarn("RpcClientPool warmup failed for " + ch->EndpointString());
      // 预热失败：直接标记不健康，避免后续仍参与选择
      ch->consecutive_failures.store(options_.max_consecutive_failures,
                                     std::memory_order_release);
      ch->healthy.store(false, std::memory_order_release);
    }
  }
  return connected > 0;
}

// ============================================================================
// 节点选择
// ============================================================================

RpcClientPool::Channel* RpcClientPool::SelectChannel() {
  return SelectChannel({});
}

RpcClientPool::Channel* RpcClientPool::SelectChannel(
    const std::vector<Channel*>& excluded) {
  // 1. 收集 healthy 且不在排除列表中的候选节点
  std::vector<Channel*> healthy_candidates;
  healthy_candidates.reserve(channels_.size());

  for (auto& ch : channels_) {
    if (ch->healthy.load(std::memory_order_acquire) &&
        std::find(excluded.begin(), excluded.end(), ch.get()) == excluded.end()) {
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
// 健康状态与统计
// ============================================================================

void RpcClientPool::RecordReachability(Channel* channel, bool reachable) {
  if (reachable) {
    // 连接可达：清零连续失败计数并标记健康
    channel->consecutive_failures.store(0, std::memory_order_release);
    channel->healthy.store(true, std::memory_order_release);
  } else {
    // 连接不可达：递增连续失败计数
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

void RpcClientPool::RecordCallResult(Channel* channel,
                                     const RpcCallResult& result) {
  if (result.ok()) {
    channel->success_count.fetch_add(1, std::memory_order_relaxed);
  } else {
    channel->fail_count.fetch_add(1, std::memory_order_relaxed);
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
// 同步 Call（支持重试，每轮排除已尝试节点）
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

  std::vector<Channel*> attempted;
  attempted.reserve(channels_.size());

  const std::size_t max_attempts = channels_.size();
  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    Channel* ch = SelectChannel(attempted);
    if (!ch) {
      return RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "no channel available"},
          {}};
    }

    attempted.push_back(ch);
    auto result = ch->client->Call(service_name, method_name, request_payload);
    RecordCallResult(ch, result);

    if (result.ok()) {
      RecordReachability(ch, true);
      return result;
    }

    // 失败处理：如果是连接类错误，标记节点不可达并尝试下一个
    if (IsConnectionError(result)) {
      RecordReachability(ch, false);
      common::LogWarn("RpcClientPool: call failed on " + ch->EndpointString() +
                      ", retrying... attempt=" + std::to_string(attempt + 1));
      continue;
    }

    // 业务错误（如 MethodNotFound）不重试，直接返回
    // 业务错误说明连接本身是好的，标记可达
    RecordReachability(ch, true);
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

  // select_count 已在 SelectChannel() 中统一计数，此处不再重复
  return ch->client->CallAsync(service_name, method_name, request_payload);
}

// ============================================================================
// 协程 CallCo（不重试，但完成后更新健康状态）
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

  // select_count 已在 SelectChannel() 中统一计数，此处不再重复
  auto result = co_await ch->client->CallCo(service_name, method_name,
                                            request_payload);

  RecordCallResult(ch, result);
  // 只有连接类错误才影响节点健康状态；业务错误不影响
  RecordReachability(ch, !IsConnectionError(result));

  co_return result;
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
