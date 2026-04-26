#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace rpc::client {

class RpcClient;

/// 负载均衡策略基类
///
/// 负责从一组候选客户端中选择一个用于发送请求。
/// 所有策略必须保证线程安全（Select 可能从多线程并发调用）。
class LoadBalancer {
 public:
  virtual ~LoadBalancer() = default;

  /// 从候选客户端中选择一个
  /// @param clients 候选 RpcClient 指针列表（已由调用方过滤为 healthy）
  /// @return 选中客户端在 candidates 中的索引
  /// @note 调用方保证 candidates 非空
  virtual std::size_t Select(
      const std::vector<RpcClient*>& candidates) = 0;

  virtual const char* Name() const = 0;
};

/// 轮询负载均衡策略
///
/// 使用原子计数器实现无锁轮询，保证多线程下的公平分发。
/// 当某个节点不健康被剔除后，轮询仍然平滑进行（基于当前 candidates 列表长度取模）。
class RoundRobinBalancer : public LoadBalancer {
 public:
  std::size_t Select(
      const std::vector<RpcClient*>& candidates) override;
  const char* Name() const override { return "RoundRobin"; }

 private:
  std::atomic<std::size_t> next_index_{0};
};

/// 最小 inflight 负载均衡策略
///
/// 每次选择当前 inflight（未响应请求数）最少的连接。
/// 通过 RpcClient::GetInflightCount() 获取实时 inflight 数，
/// 无需 Pool 层自行维护计数，保证精确且避免重复统计。
///
/// ## 为什么需要 tie-break？
///
/// 在轻载或空闲场景下，大多数节点 inflight 同时为 0。
/// 如果不做 tie-break，会长期偏向第一个候选节点，
/// 导致负载均衡退化为“总是选第一个”。
/// 因此我们在 inflight 相同的节点子集内再做一轮原子轮询，
/// 既保持最小 inflight 语义，又避免热点。
class LeastInflightBalancer : public LoadBalancer {
 public:
  std::size_t Select(
      const std::vector<RpcClient*>& candidates) override;
  const char* Name() const override { return "LeastInflight"; }

 private:
  /// 当多个节点 inflight 相同时，用原子计数器做轮询打散。
  std::atomic<std::size_t> tie_breaker_{0};
};

}  // namespace rpc::client
