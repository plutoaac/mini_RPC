#include "client/load_balancer.h"

#include <limits>
#include <vector>

#include "client/rpc_client.h"

namespace rpc::client {

std::size_t RoundRobinBalancer::Select(
    const std::vector<RpcClient*>& candidates) {
  // fetch_add 返回旧值，因此直接对 candidates.size() 取模即可得到本次索引
  const std::size_t index =
      next_index_.fetch_add(1, std::memory_order_relaxed) % candidates.size();
  return index;
}

std::size_t LeastInflightBalancer::Select(
    const std::vector<RpcClient*>& candidates) {
  // 第一步：找出最小 inflight 值
  std::size_t min_inflight = std::numeric_limits<std::size_t>::max();
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const std::size_t inflight = candidates[i]->GetInflightCount();
    if (inflight < min_inflight) {
      min_inflight = inflight;
    }
  }

  // 第二步：收集所有 inflight 等于最小值的候选索引
  std::vector<std::size_t> tie_indices;
  tie_indices.reserve(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i]->GetInflightCount() == min_inflight) {
      tie_indices.push_back(i);
    }
  }

  // 第三步：在 tie 子集中用原子轮询做打散，避免长期偏向第一个节点
  const std::size_t tie_index =
      tie_breaker_.fetch_add(1, std::memory_order_relaxed) % tie_indices.size();
  return tie_indices[tie_index];
}

}  // namespace rpc::client
