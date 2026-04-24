#include "client/load_balancer.h"

#include <limits>

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
  std::size_t best_index = 0;
  std::size_t min_inflight = std::numeric_limits<std::size_t>::max();

  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const std::size_t inflight = candidates[i]->GetInflightCount();
    if (inflight < min_inflight) {
      min_inflight = inflight;
      best_index = i;
      // 最优情况：已经找到 inflight 为 0 的节点，无需继续遍历
      if (min_inflight == 0) {
        break;
      }
    }
  }
  return best_index;
}

}  // namespace rpc::client
