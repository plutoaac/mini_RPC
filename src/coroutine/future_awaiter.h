/// @file future_awaiter.h
/// @brief 将 std::future 包装为可 co_await 的协程类型
///
/// 该文件提供了 FutureAwaiter 类模板，用于将传统的 std::future
/// 适配到 C++20 协程模型中，使得异步代码可以以同步风格编写。
///
/// @example
/// @code
/// std::future<int> async_compute();
///
/// rpc::coroutine::Task<int> compute() {
///   // co_await 一个 std::future
///   int result = co_await rpc::coroutine::FromFuture(async_compute());
///   co_return result;
/// }
/// @endcode

#pragma once

#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace rpc::coroutine {

/// @brief 将 std::future<T> 包装为可 co_await 的 Awaiter 类型
/// @tparam T future 的结果类型
///
/// FutureAwaiter 实现了协程 Awaiter 接口的三个核心方法：
/// - await_ready(): 检查 future 是否已完成
/// - await_suspend(): 在后台线程等待 future 完成
/// - await_resume(): 获取 future 的结果
///
/// 工作流程：
/// 1. await_ready() 检查 future 是否已就绪
/// 2. 若未就绪，await_suspend() 启动后台线程等待
/// 3. 后台线程完成后恢复协程执行
/// 4. await_resume() 返回结果或重新抛出异常
template <typename T>
class FutureAwaiter {
 public:
  /// @brief 构造函数
  /// @param future 要包装的 std::future 对象
  explicit FutureAwaiter(std::future<T> future)
      : state_(std::make_shared<State>(std::move(future))) {}

  /// @brief 检查 future 是否已就绪
  /// @return true 表示已就绪，无需挂起协程
  ///
  /// 使用 wait_for(0) 非阻塞地检查 future 状态，
  /// 如果已完成则直接在当前线程获取结果，避免不必要的挂起开销。
  bool await_ready() const {
    return state_->future.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
  }

  /// @brief 挂起协程并启动后台线程等待 future 完成
  /// @param handle 当前协程的句柄，用于恢复执行
  ///
  /// 该方法启动一个分离的后台线程来等待 future 完成。
  /// 使用分离线程的原因：
  /// 1. 避免阻塞当前线程
  /// 2. std::future 不支持回调机制，只能阻塞等待
  /// 3. 协程可以在此期间执行其他工作
  ///
  /// 注意：后台线程会在 future 完成后调用 handle.resume() 恢复协程。
  void await_suspend(std::coroutine_handle<> handle) {
    auto state = state_;
    // 启动分离线程等待 future 完成
    std::thread([state = std::move(state), handle]() mutable {
      try {
        // 阻塞等待 future 完成并获取结果
        state->value.emplace(state->future.get());
      } catch (...) {
        // 捕获异常，稍后在 await_resume 中重新抛出
        state->exception = std::current_exception();
      }
      // 恢复协程执行
      handle.resume();
    }).detach();
  }

  /// @brief 恢复协程时获取结果
  /// @return future 中存储的值
  /// @throws 若 future 抛出异常，此处重新抛出
  ///
  /// 协程恢复后调用此方法获取 await 表达式的结果。
  T await_resume() {
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
    return std::move(*(state_->value));
  }

 private:
  /// @brief 内部状态结构，在线程间共享
  ///
  /// 使用 shared_ptr 管理 State 的原因：
  /// 1. State 需要在主线程和后台线程间共享
  /// 2. future 只能移动，不能拷贝
  /// 3. value 和 exception 需要跨线程传递
  struct State {
    explicit State(std::future<T> f) : future(std::move(f)) {}

    std::future<T> future;         ///< 被等待的 future
    std::optional<T> value;        ///< future 完成后存储结果
    std::exception_ptr exception;  ///< 存储可能发生的异常
  };

  /// @brief 共享状态指针
  /// 使用 shared_ptr 确保 State 在后台线程完成前不会被销毁
  std::shared_ptr<State> state_;
};

/// @brief void 类型特化，用于处理 std::future<void>
///
/// void future 不返回值，只需等待完成或捕获异常。
/// 其他逻辑与通用模板相同。
template <>
class FutureAwaiter<void> {
 public:
  explicit FutureAwaiter(std::future<void> future)
      : state_(std::make_shared<State>(std::move(future))) {}

  /// @brief 检查 void future 是否已就绪
  bool await_ready() const {
    return state_->future.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
  }

  /// @brief 挂起协程等待 void future 完成
  void await_suspend(std::coroutine_handle<> handle) {
    auto state = state_;
    std::thread([state = std::move(state), handle]() mutable {
      try {
        state->future.get();  // 等待完成，无返回值
      } catch (...) {
        state->exception = std::current_exception();
      }
      handle.resume();
    }).detach();
  }

  /// @brief 恢复协程，可能重新抛出异常
  void await_resume() {
    if (state_->exception) {
      std::rethrow_exception(state_->exception);
    }
  }

 private:
  struct State {
    explicit State(std::future<void> f) : future(std::move(f)) {}

    std::future<void> future;
    std::exception_ptr exception;
  };

  std::shared_ptr<State> state_;
};

/// @brief 工厂函数：从 std::future 创建 FutureAwaiter
/// @tparam T future 的结果类型
/// @param future 要包装的 future 对象
/// @return 可 co_await 的 FutureAwaiter 对象
///
/// 这是推荐的创建 FutureAwaiter 的方式，语法更清晰：
/// @code
/// auto result = co_await FromFuture(my_future);
/// @endcode
template <typename T>
FutureAwaiter<T> FromFuture(std::future<T> future) {
  return FutureAwaiter<T>(std::move(future));
}

}  // namespace rpc::coroutine