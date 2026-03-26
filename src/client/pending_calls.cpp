/// @file pending_calls.cpp
/// @brief RPC 调用请求管理器实现
///
/// 本文件实现了 PendingCalls 类，用于管理进行中的 RPC 调用请求。
///
/// ## 核心改进点（针对协程 double free 问题）
///
/// ### 改进 1：锁外恢复协程（Lock-Outside-Resume 模式）
///
/// 在 Complete()、FailAll()、FailTimedOut() 中，采用"锁内摘句柄，锁外 resume"的模式：
/// - 在锁保护下取出 coroutine_handle，并立即解绑槽位
/// - 释放锁后，再执行 coroutine_to_resume.resume()
///
/// 好处：
/// 1. 避免"持锁 resume"导致的锁重入问题（协程恢复后可能尝试再次获取同一把锁）
/// 2. 减少锁持有时间，降低阻塞放大
/// 3. 防止死锁：如果协程恢复后在 await_resume 中调用 TryPop，会需要获取同一把锁
///
/// ### 改进 2：协程槽位先解绑再恢复
///
/// 在取出 coroutine_handle 后，立即执行：
/// ```cpp
/// slot.coroutine_bound = false;
/// slot.coroutine_handle = {};
/// ```
///
/// 好处：
/// 1. 防止重复恢复：如果超时线程同时也在处理该请求，解绑后不会重复 resume
/// 2. 明确所有权转移：句柄取出后，槽位不再持有引用，生命周期由恢复方管理
///
/// ### 改进 3：异步与协程互斥绑定
///
/// Slot 同时支持三种等待模式，但同一时间只能绑定一种：
/// - 同步等待（cv + WaitAndPop）
/// - 异步等待（async_promise + CallAsync）
/// - 协程等待（coroutine_handle + CallCo）
///
/// BindAsync() 和 BindCoroutine() 会检查对方是否已绑定，拒绝冲突绑定。
/// 这避免了"一个请求被多个等待者监听"的复杂竞态。

#include "client/pending_calls.h"

#include <chrono>
#include <utility>
#include <vector>

namespace rpc::client {

/// 添加一个新的请求槽位
///
/// 使用 try_emplace 保证原子性：
/// - 如果 key 不存在，创建新的 Slot（默认 done=false）
/// - 如果 key 已存在，不做任何修改
///
/// @note 调用方应确保 request_id 唯一，通常由 RpcClient::NextRequestId() 生成
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

/// 绑定异步等待者（CallAsync 路径）
///
/// @param request_id 请求 ID
/// @param promise 用于设置结果的 promise
/// @param deadline 请求超时时间点
/// @return 绑定成功返回 true
///
/// @note 如果响应已先到达（done=true），会立即完成 promise 并移除槽位，
///       这是一种"快路径"优化，避免不必要的等待。
bool PendingCalls::BindAsync(std::string_view request_id,
                             std::promise<RpcCallResult> promise,
                             std::chrono::steady_clock::time_point deadline) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    return false;
  }

  // 互斥检查：已被异步绑定则拒绝
  if (it->second.async_bound) {
    return false;
  }

  // 互斥检查：已被协程绑定则拒绝
  if (it->second.coroutine_bound) {
    return false;
  }

  // 快路径：响应已到达，立即完成并返回
  if (it->second.done) {
    promise.set_value(std::move(it->second.result));
    slots_.erase(it);
    return true;
  }

  // 慢路径：绑定等待者，等待 Complete() 唤醒
  it->second.async_promise.emplace(std::move(promise));
  it->second.async_bound = true;
  it->second.deadline = deadline;
  return true;
}

/// 绑定协程等待者（CallCo 路径）
///
/// @param request_id 请求 ID
/// @param handle 协程句柄，用于在响应到达时恢复执行
/// @param deadline 请求超时时间点
/// @return 绑定状态枚举
///
/// @note 返回 kAlreadyDone 表示响应已就绪，调用方不应挂起协程，
///       应直接在 await_resume 中获取结果。
PendingCalls::BindCoroutineStatus PendingCalls::BindCoroutine(
    std::string_view request_id, std::coroutine_handle<> handle,
    std::chrono::steady_clock::time_point deadline) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    return BindCoroutineStatus::kNotFound;
  }

  // 互斥检查：已被协程绑定则拒绝
  if (it->second.coroutine_bound) {
    return BindCoroutineStatus::kAlreadyBound;
  }

  // 互斥检查：已被异步绑定则拒绝
  if (it->second.async_bound) {
    return BindCoroutineStatus::kAlreadyBound;
  }

  // 快路径：响应已到达，告知调用方不要挂起
  // 调用方会在 await_resume 中调用 TryPop 获取结果
  if (it->second.done) {
    return BindCoroutineStatus::kAlreadyDone;
  }

  // 慢路径：绑定协程句柄，等待 Complete() 恢复
  it->second.coroutine_handle = handle;
  it->second.coroutine_bound = true;
  it->second.deadline = deadline;
  return BindCoroutineStatus::kBound;
}

/// 完成一个请求并设置结果
///
/// 由 Dispatcher 线程调用。找到对应的槽位，设置结果并唤醒等待者。
///
/// ## 关键设计：锁外恢复协程
///
/// 这里采用了"先摘句柄，后释放锁，再 resume"的模式：
///
/// ```cpp
/// {
///   std::scoped_lock lock(mutex_);
///   // ... 在锁内完成状态更新和句柄取出 ...
///   coroutine_to_resume = slot.coroutine_handle;
///   slot.coroutine_bound = false;  // 立即解绑
///   slot.coroutine_handle = {};    // 清空句柄
/// }  // 锁释放
///
/// if (coroutine_to_resume) {
///   coroutine_to_resume.resume();  // 锁外恢复，避免重入问题
/// }
/// ```
///
/// 这样做的原因：
/// 1. 协程恢复后会执行 await_resume() → TryPop()，TryPop 需要获取同一把锁
/// 2. 如果在锁内 resume，会导致"持锁等待自己释放锁"的死锁
/// 3. 锁外 resume 让协程可以安全地访问 PendingCalls 的其他方法
///
/// @param request_id 请求唯一标识符
/// @param result RPC 调用结果
/// @return 找到并完成成功返回 true
bool PendingCalls::Complete(std::string_view request_id, RpcCallResult result) {
  // === 声明锁外使用的变量 ===
  std::coroutine_handle<> coroutine_to_resume;  // 待恢复的协程句柄
  std::optional<std::promise<RpcCallResult>> async_promise;  // 待完成的异步 promise
  RpcCallResult async_result;  // 异步结果

  {
    std::scoped_lock lock(mutex_);

    // 查找对应的槽位
    const auto it = slots_.find(std::string(request_id));
    if (it == slots_.end()) {
      // 请求不存在，可能已超时被移除
      return false;
    }

    Slot& slot = it->second;
    if (slot.done) {
      // 已经完成（可能是超时后重复响应），忽略
      return false;
    }

    // === 设置完成状态和结果 ===
    slot.done = true;
    slot.result = std::move(result);

    // === 根据等待者类型选择唤醒方式 ===

    if (slot.async_promise.has_value()) {
      // 异步等待模式：取出 promise，稍后在锁外 set_value
      // 异步请求在 set_value 后可以直接移除槽位
      async_result = slot.result;
      async_promise = std::move(slot.async_promise);
      slots_.erase(it);
    } else if (slot.coroutine_bound && slot.coroutine_handle) {
      // 协程等待模式：取出句柄，稍后在锁外 resume
      //
      // 【关键】先解绑再恢复，防止：
      // 1. 超时线程同时也在处理该请求时重复 resume
      // 2. 明确所有权转移，槽位不再持有句柄引用
      coroutine_to_resume = slot.coroutine_handle;
      slot.coroutine_bound = false;
      slot.coroutine_handle = {};
      // 注意：此时不删除槽位，因为协程恢复后会在 await_resume 中调用 TryPop 取结果
    } else {
      // 同步等待模式：唤醒阻塞在 WaitAndPop 的线程
      slot.cv.notify_all();
    }
  }  // === 锁释放 ===

  // === 锁外执行耗时/阻塞操作 ===

  if (async_promise.has_value()) {
    // 设置 promise 结果，唤醒 CallAsync 的等待线程
    async_promise->set_value(std::move(async_result));
  }

  if (coroutine_to_resume) {
    // 【关键】锁外恢复协程，避免重入死锁
    // 协程恢复后会执行 await_resume → TryPop，TryPop 需要获取锁
    // 如果在锁内 resume，会死锁
    coroutine_to_resume.resume();
  }

  return true;
}

/// 等待请求完成并取出结果
///
/// 阻塞等待直到收到响应或超时。
/// 使用条件变量实现高效的等待，避免忙等。
///
/// @param request_id 请求唯一标识符
/// @param timeout 超时时间
/// @return 成功返回结果，超时或请求不存在返回 nullopt
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
/// 立即尝试获取结果，不等待。用于协程恢复后的结果获取。
///
/// @param request_id 请求唯一标识符
/// @return 已完成返回结果，未完成或不存在返回 nullopt
///
/// @note 这是 CallCo 路径中 await_resume 调用的方法。
///       Complete() 已预先设置了 done=true 和 result，这里只需取出。
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
///
/// @param request_id 请求唯一标识符
/// @note 如果有线程正在等待该请求，删除后等待会返回 nullopt
void PendingCalls::Remove(std::string_view request_id) {
  std::scoped_lock lock(mutex_);
  slots_.erase(std::string(request_id));
}

/// 将所有待处理请求标记为失败
///
/// 当连接断开或发生严重错误时调用。
/// 遍历所有槽位，设置错误结果并唤醒等待者。
///
/// @param result_template 失败结果模板，将应用于所有请求
///
/// @note 同样采用"锁内摘句柄，锁外 resume"模式，避免死锁。
void PendingCalls::FailAll(const RpcCallResult& result_template) {
  // === 声明锁外使用的容器 ===
  std::vector<std::coroutine_handle<>> coroutine_waiters;
  std::vector<std::pair<std::promise<RpcCallResult>, RpcCallResult>>
      async_waiters;

  {
    std::scoped_lock lock(mutex_);

    for (auto& [_, slot] : slots_) {
      // 已经完成并写入结果的请求不应被覆盖，避免把成功结果改写为失败。
      if (!slot.done) {
        slot.done = true;
        slot.result = result_template;
      }

      // 异步等待者：取出 promise，锁外 set_value
      if (slot.async_promise.has_value()) {
        async_waiters.emplace_back(std::move(*(slot.async_promise)),
                                   slot.result);
        slot.async_promise.reset();
      }

      // 协程等待者：取出句柄，锁外 resume
      // 【关键】先解绑再收集，防止重复恢复
      if (slot.coroutine_bound && slot.coroutine_handle) {
        coroutine_waiters.push_back(slot.coroutine_handle);
        slot.coroutine_bound = false;
        slot.coroutine_handle = {};
      }

      // 无论是否已完成，都通知同步等待线程尽快退出等待。
      slot.cv.notify_all();
    }
  }  // === 锁释放 ===

  // === 锁外执行唤醒操作 ===
  for (auto& waiter : async_waiters) {
    waiter.first.set_value(std::move(waiter.second));
  }
  for (auto handle : coroutine_waiters) {
    handle.resume();
  }
}

/// 使已到 deadline 的异步请求失败
///
/// 由 dispatcher 在读超时分支中调用，避免每请求一个等待线程。
///
/// @param now 当前时间点
/// @param timeout_result 超时错误结果
///
/// @note 同样采用"锁内摘句柄，锁外 resume"模式。
void PendingCalls::FailTimedOut(std::chrono::steady_clock::time_point now,
                                const RpcCallResult& timeout_result) {
  // === 声明锁外使用的容器 ===
  std::vector<std::coroutine_handle<>> coroutine_waiters;
  std::vector<std::pair<std::promise<RpcCallResult>, RpcCallResult>>
      async_waiters;

  {
    std::scoped_lock lock(mutex_);

    for (auto& [_, slot] : slots_) {
      // 跳过：已完成、未到超时、非异步/协程等待的请求
      if (slot.done || now < slot.deadline ||
          (!slot.async_bound && !slot.coroutine_bound)) {
        continue;
      }

      // 标记超时
      slot.done = true;
      slot.result = timeout_result;

      // 异步等待者：收集 promise
      if (slot.async_promise.has_value()) {
        async_waiters.emplace_back(std::move(*(slot.async_promise)),
                                   slot.result);
        slot.async_promise.reset();
      }

      // 协程等待者：收集句柄
      // 【关键】先解绑再收集，防止重复恢复
      if (slot.coroutine_bound && slot.coroutine_handle) {
        coroutine_waiters.push_back(slot.coroutine_handle);
        slot.coroutine_bound = false;
        slot.coroutine_handle = {};
      }

      slot.cv.notify_all();
    }
  }  // === 锁释放 ===

  // === 锁外执行唤醒操作 ===
  for (auto& waiter : async_waiters) {
    waiter.first.set_value(std::move(waiter.second));
  }
  for (auto handle : coroutine_waiters) {
    handle.resume();
  }
}

/// 获取当前待处理请求数量
std::size_t PendingCalls::Size() const {
  std::scoped_lock lock(mutex_);
  return slots_.size();
}

}  // namespace rpc::client
