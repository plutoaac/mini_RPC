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

  // 防御性检查：TOCTOU 可能导致 tie_indices 为空
  // （步骤 1 和步骤 2 之间 inflight 可能变化），此时回退到第一个候选
  if (tie_indices.empty()) {
    // 重新找当前最小值（精确但不会为空）
    min_inflight = std::numeric_limits<std::size_t>::max();
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const std::size_t inflight = candidates[i]->GetInflightCount();
      if (inflight < min_inflight) {
        min_inflight = inflight;
        tie_indices.clear();
        tie_indices.push_back(i);
      } else if (inflight == min_inflight) {
        tie_indices.push_back(i);
      }
    }
    // 最后的兜底：确保至少有一个候选
    if (tie_indices.empty() && !candidates.empty()) {
      tie_indices.push_back(0);
    }
  }

  // 第三步：在 tie 子集中用原子轮询做打散，避免长期偏向第一个节点
  const std::size_t tie_index =
      tie_breaker_.fetch_add(1, std::memory_order_relaxed) % tie_indices.size();
  return tie_indices[tie_index];
}

}  // namespace rpc::client
