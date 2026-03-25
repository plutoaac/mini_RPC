#include "client/pending_calls.h"

#include <chrono>

namespace rpc::client {

/// 添加一个新的请求槽位
///
/// 使用 try_emplace 保证原子性：
/// - 如果 key 不存在，创建新的 Slot（默认 done=false）
/// - 如果 key 已存在，不做任何修改
bool PendingCalls::Add(std::string request_id) {
  // 拒绝空的 request_id
  if (request_id.empty()) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  // try_emplace 返回 pair<iterator, bool>
  // second 为 true 表示插入成功
  return slots_.try_emplace(std::move(request_id)).second;
}

bool PendingCalls::BindAsync(std::string_view request_id,
                             std::promise<RpcCallResult> promise,
                             std::chrono::steady_clock::time_point deadline) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    return false;
  }

  if (it->second.async_bound) {
    return false;
  }

  if (it->second.done) {
    promise.set_value(std::move(it->second.result));
    slots_.erase(it);
    return true;
  }

  it->second.async_promise.emplace(std::move(promise));
  it->second.async_bound = true;
  it->second.deadline = deadline;
  return true;
}

/// 完成一个请求并设置结果
///
/// 由 Dispatcher 线程调用。找到对应的槽位，设置结果并唤醒等待者。
bool PendingCalls::Complete(std::string_view request_id, RpcCallResult result) {
  std::scoped_lock lock(mutex_);
  // 查找对应的槽位
  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    // 请求不存在，可能已超时被移除
    return false;
  }

  Slot& slot = it->second;

  // 设置完成状态和结果
  slot.done = true;
  slot.result = std::move(result);

  // 异步请求直接完成 future，并在 pending 表中移除。
  if (slot.async_promise.has_value()) {
    slot.async_promise->set_value(slot.result);
    slots_.erase(it);
    return true;
  }

  // 同步等待模式下唤醒等待线程。
  slot.cv.notify_all();
  return true;
}

/// 等待请求完成并取出结果
///
/// 阻塞等待直到收到响应或超时。
/// 使用条件变量实现高效的等待，避免忙等。
std::optional<RpcCallResult> PendingCalls::WaitAndPop(
    std::string_view request_id, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);

  const auto key = std::string(request_id);
  auto it = slots_.find(key);
  if (it == slots_.end()) {
    // 请求不存在
    return std::nullopt;
  }

  auto& slot = it->second;
  // 如果尚未完成，等待条件变量
  if (!slot.done) {
    // wait_for 返回 false 表示超时
    const bool ready =
        slot.cv.wait_for(lock, timeout, [&slot] { return slot.done; });
    if (!ready) {
      // 超时返回空
      return std::nullopt;
    }
    // 被唤醒后重新查找，因为槽位可能在等待期间被删除
    it = slots_.find(key);
    if (it == slots_.end()) {
      return std::nullopt;
    }
  }

  // 取出结果并移除槽位
  RpcCallResult result = std::move(it->second.result);
  slots_.erase(it);
  return result;
}

/// 非阻塞获取已完成结果
///
/// 立即尝试获取结果，不等待。用于错误处理路径的快速清理。
std::optional<RpcCallResult> PendingCalls::TryPop(std::string_view request_id) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));
  // 检查是否存在且已完成
  if (it == slots_.end() || !it->second.done) {
    return std::nullopt;
  }

  // 取出结果并移除槽位
  RpcCallResult result = std::move(it->second.result);
  slots_.erase(it);
  return result;
}

/// 删除某个请求槽位
///
/// 用于调用超时后的清理，防止槽位泄漏。
void PendingCalls::Remove(std::string_view request_id) {
  std::scoped_lock lock(mutex_);
  slots_.erase(std::string(request_id));
}

/// 将所有待处理请求标记为失败
///
/// 当连接断开或发生严重错误时调用。
/// 遍历所有槽位，设置错误结果并唤醒等待者。
void PendingCalls::FailAll(const RpcCallResult& result_template) {
  std::scoped_lock lock(mutex_);
  for (auto& [_, slot] : slots_) {
    // 已经完成并写入结果的请求不应被覆盖，避免把成功结果改写为失败。
    if (!slot.done) {
      slot.done = true;
      slot.result = result_template;
    }
    if (slot.async_promise.has_value()) {
      slot.async_promise->set_value(slot.result);
      slot.async_promise.reset();
    }

    // 无论是否已完成，都通知同步等待线程尽快退出等待。
    slot.cv.notify_all();
  }
}

void PendingCalls::FailTimedOut(std::chrono::steady_clock::time_point now,
                                const RpcCallResult& timeout_result) {
  std::scoped_lock lock(mutex_);

  for (auto it = slots_.begin(); it != slots_.end();) {
    Slot& slot = it->second;
    if (!slot.async_bound || slot.done || now < slot.deadline) {
      ++it;
      continue;
    }

    slot.done = true;
    slot.result = timeout_result;

    if (slot.async_promise.has_value()) {
      slot.async_promise->set_value(slot.result);
    }

    it = slots_.erase(it);
  }
}

/// 获取当前待处理请求数量
std::size_t PendingCalls::Size() const {
  std::scoped_lock lock(mutex_);
  return slots_.size();
}

}  // namespace rpc::client