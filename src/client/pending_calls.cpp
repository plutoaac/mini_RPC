#include "client/pending_calls.h"

namespace rpc::client {

/// 添加一个待处理的请求槽位
///
/// 调用时机：在发送 RPC 请求之前
/// 目的：为该请求预留一个结果存储位置，等待后续响应填充
bool PendingCalls::Add(std::string request_id) {
  // 参数校验：空 request_id 是非法的
  if (request_id.empty()) {
    return false;
  }

  // 加锁保护 slots_ 的并发访问
  std::scoped_lock lock(mutex_);

  // emplace 返回 pair<iterator, bool>
  // .second 为 true 表示插入成功，false 表示 key 已存在（重复 request_id）
  return slots_.emplace(std::move(request_id), Slot{}).second;
}

/// 完成一个请求，填充结果
///
/// 调用时机：收到服务端响应时
/// 目的：将响应结果写入对应的槽位，通知等待者结果已就绪
bool PendingCalls::Complete(std::string_view request_id, RpcCallResult result) {
  std::scoped_lock lock(mutex_);

  // 查找对应的槽位
  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    // 找不到槽位，可能：
    // 1. request_id 非法
    // 2. 该请求已被 Pop 移除
    // 3. 服务端返回了未发送过的请求ID（异常情况）
    return false;
  }

  // 标记为已完成，并存储结果
  it->second.done = true;
  it->second.result = std::move(result);
  return true;
}

/// 弹出已完成的请求结果
///
/// 调用时机：等待响应的线程轮询检查结果
/// 返回值：
///   - nullopt：请求未找到 或 尚未完成
///   - 结果：请求已完成，返回结果并移除槽位
std::optional<RpcCallResult> PendingCalls::Pop(std::string_view request_id) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));

  // 槽位不存在，或请求尚未完成
  if (it == slots_.end() || !it->second.done) {
    return std::nullopt;
  }

  // 取出结果，并移除槽位
  // 移除后，后续对该 request_id 的 Pop/Complete 都会失败
  RpcCallResult result = std::move(it->second.result);
  slots_.erase(it);
  return result;
}

/// 将所有待处理请求标记为失败
///
/// 调用时机：
/// - 连接断开时
/// - 客户端关闭时
/// - 发生不可恢复的错误时
///
/// 目的：让所有等待中的请求都能得到一个失败响应，避免无限等待
void PendingCalls::FailAll(const RpcCallResult& result_template) {
  std::scoped_lock lock(mutex_);

  // 遍历所有槽位，标记为完成并填充失败结果
  for (auto& [_, slot] : slots_) {
    slot.done = true;
    slot.result = result_template;
  }
  // 注意：这里没有清除 slots_，让等待者可以通过 Pop 获取失败结果
}

/// 获取当前待处理的请求数量
std::size_t PendingCalls::Size() const {
  std::scoped_lock lock(mutex_);
  return slots_.size();
}

}  // namespace rpc::client
