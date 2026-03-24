#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "client/rpc_types.h"

namespace rpc::client {

// PendingCalls 用于管理 request_id -> result slot。
// 支持“调用线程等待 + 响应线程分发”的并发模型。
class PendingCalls {
 public:
  [[nodiscard]] bool Add(std::string request_id);

  [[nodiscard]] bool Complete(std::string_view request_id,
                              RpcCallResult result);

  // 等待某个请求完成并取出结果；超时返回 nullopt。
  [[nodiscard]] std::optional<RpcCallResult> WaitAndPop(
      std::string_view request_id, std::chrono::milliseconds timeout);

  // 非阻塞获取已完成结果；未完成或不存在返回 nullopt。
  [[nodiscard]] std::optional<RpcCallResult> TryPop(
      std::string_view request_id);

  // 删除某个请求槽位（用于调用超时清理）。
  void Remove(std::string_view request_id);

  // 将所有待处理请求标记为失败，并唤醒等待者。
  void FailAll(const RpcCallResult& result_template);

  [[nodiscard]] std::size_t Size() const;

 private:
  struct Slot {
    bool done{false};
    RpcCallResult result;
    std::condition_variable cv;
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Slot> slots_;
};

}  // namespace rpc::client
