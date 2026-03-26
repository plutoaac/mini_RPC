/// @file task.h
/// @brief 协程 Task 类型，用于表示可 co_await 的异步任务
///
/// Task 是 C++20 协程的返回类型包装器，支持：
/// - 协程间相互 co_await（链式调用）
/// - 异常传播
/// - 同步等待结果
///
/// @example
/// @code
/// // 定义一个返回 int 的协程
/// rpc::coroutine::Task<int> async_add(int a, int b) {
///   co_return a + b;
/// }
///
/// // 在另一个协程中 co_await
/// rpc::coroutine::Task<int> compute() {
///   int result = co_await async_add(1, 2);
///   co_return result * 2;  // 返回 6
/// }
///
/// // 同步等待协程完成
/// int main() {
///   auto task = compute();
///   std::cout << task.Get() << std::endl;  // 输出 6
/// }
/// @endcode

#pragma once

#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <optional>
#include <utility>

namespace rpc::coroutine {

/// @brief 协程任务包装器
/// @tparam T 协程返回值类型
///
/// Task 实现了协程的 promise_type，这是 C++20 协程机制要求的接口。
/// 每个 Task 对象持有对应的协程句柄，负责管理协程的生命周期。
///
/// 设计要点：
/// 1. 使用 std::shared_future 支持多次获取结果
/// 2. 支持协程链式调用（continuation 机制）
/// 3. RAII 管理协程句柄生命周期
template <typename T>
class Task {
 public:
  /// @brief promise_type 是协程机制要求的类型定义
  ///
  /// 编译器在生成协程代码时会：
  /// 1. 使用 promise_type 创建 promise 对象
  /// 2. 通过 get_return_object() 创建 Task 返回值
  /// 3. 在 co_return 时调用 return_value()
  /// 4. 协程结束时调用 final_suspend()
  struct promise_type {
    std::promise<T> completion_promise;  ///< 用于设置协程结果
    std::shared_future<T>
        completion_future;                 ///< 用于获取协程结果（可多次获取）
    std::coroutine_handle<> continuation;  ///< 调用者协程句柄（用于恢复调用者）
    std::optional<T> result_value;         ///< 暂存 co_return 值，final_suspend 再发布
    std::exception_ptr unhandled_error;    ///< 暂存未捕获异常，final_suspend 再发布
    bool completion_set{false};            ///< 防止重复 set_value/set_exception

    /// @brief 构造函数，初始化 shared_future
    promise_type()
        : completion_future(completion_promise.get_future().share()) {}

    /// @brief 创建 Task 返回对象
    /// @return 包装了协程句柄的 Task 对象
    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /// @brief 协程启动时的行为
    /// @return std::suspend_never 表示不挂起，立即开始执行协程体
    std::suspend_never initial_suspend() noexcept { return {}; }

    /// @brief 协程结束时的 Awaiter
    ///
    /// FinalAwaiter 负责在协程结束时恢复调用者协程（如果存在），
    /// 实现协程链式调用的"回调"机制。
    struct FinalAwaiter {
      /// @brief 是否已就绪
      /// @return false，表示需要挂起（执行 await_suspend）
      bool await_ready() const noexcept { return false; }

      /// @brief 挂起时的处理：恢复调用者协程
      /// @param handle 当前协程（即将销毁）的句柄
      ///
      /// 当协程执行完毕后，如果有 continuation（调用者在 await 此协程），
      /// 则恢复调用者的执行。
      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<promise_type> handle) noexcept {
        auto& promise = handle.promise();
        promise.PublishCompletion();

        auto continuation = promise.continuation;
        // 对称转移：避免在当前协程 final_suspend 栈帧内直接 resume continuation
        // 造成重入销毁协程帧的未定义行为。
        if (continuation) {
          return continuation;
        }
        return std::noop_coroutine();
      }

      /// @brief 恢复时无操作
      void await_resume() const noexcept {}
    };

    /// @brief 协程结束时的行为
    /// @return FinalAwaiter，负责恢复调用者协程
    FinalAwaiter final_suspend() noexcept { return {}; }

    /// @brief 处理 co_return value 语句
    /// @param value 协程返回的值
    ///
    /// 当协程执行 co_return value 时，编译器调用此方法。
    /// 将值设置到 promise 中，使得等待者可以通过 future 获取结果。
    void return_value(T value) {
      result_value = std::move(value);
    }

    /// @brief 处理协程中的未捕获异常
    ///
    /// 异常会被存储到 promise 中，等待者获取结果时会重新抛出。
    void unhandled_exception() {
      unhandled_error = std::current_exception();
    }

    void PublishCompletion() noexcept {
      if (completion_set) {
        return;
      }
      completion_set = true;

      if (unhandled_error != nullptr) {
        completion_promise.set_exception(unhandled_error);
        return;
      }

      completion_promise.set_value(std::move(*result_value));
    }
  };

  /// @brief 禁止默认构造，Task 必须关联协程句柄
  Task() = delete;

  /// @brief 构造函数
  /// @param handle 协程句柄
  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  /// @brief 移动构造函数
  /// @param other 源对象
  Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = {}; }

  /// @brief 移动赋值运算符
  /// @param other 源对象
  /// @return *this
  Task& operator=(Task&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    Reset();
    handle_ = other.handle_;
    other.handle_ = {};
    return *this;
  }

  /// @brief 禁止拷贝（协程句柄是唯一的）
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  /// @brief 析构函数，确保协程正确销毁
  ~Task() { Reset(); }

  /// @brief co_await Task 时返回的 Awaiter
  ///
  /// Awaiter 实现了协程等待的三个核心方法，使得一个 Task 可以被另一个协程等待。
  class Awaiter {
   public:
    explicit Awaiter(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}

    /// @brief 检查被等待的 Task 是否已完成
    /// @return true 表示已完成，无需挂起调用者
    bool await_ready() const {
      return handle_.promise().completion_future.wait_for(
                 std::chrono::seconds(0)) == std::future_status::ready;
    }

    /// @brief 挂起调用者，注册 continuation
    /// @param awaiting 调用者协程的句柄
    /// @return true 表示需要挂起调用者
    ///
    /// 此方法设置 continuation，使得被等待的 Task 完成后能恢复调用者。
    bool await_suspend(std::coroutine_handle<> awaiting) {
      auto& promise = handle_.promise();
      promise.continuation = awaiting;
      // 如果 Task 尚未完成，返回 true 挂起调用者
      return promise.completion_future.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready;
    }

    /// @brief 恢复调用者时获取 Task 的结果
    /// @return Task 的返回值
    /// @throws 如果 Task 中有异常，此处重新抛出
    T await_resume() { return handle_.promise().completion_future.get(); }

   private:
    std::coroutine_handle<promise_type> handle_;
  };

  /// @brief 左值引用的 co_await 操作符
  /// @return Awaiter 对象，支持 co_await task
  auto operator co_await() & { return Awaiter{handle_}; }

  /// @brief 右值引用的 co_await 操作符
  /// @return Awaiter 对象，支持 co_await std::move(task)
  auto operator co_await() && { return Awaiter{handle_}; }

  /// @brief 同步等待 Task 完成并获取结果
  /// @return Task 的返回值
  /// @throws 如果 Task 中有异常，此处抛出
  ///
  /// 此方法会阻塞当前线程直到 Task 完成。
  /// 通常用于在非协程上下文中获取协程结果。
  T Get() { return handle_.promise().completion_future.get(); }

 private:
  /// @brief 重置 Task，销毁关联的协程
  ///
  /// 在销毁协程前，检查协程是否已执行完毕。
  /// 如果协程已完成，直接销毁协程帧。
  /// 如果协程未完成（不应该发生），等待其完成。
  void Reset() {
    if (!handle_) {
      return;
    }

    // 检查协程是否已完成
    if (handle_.promise().completion_future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      // 协程未完成，等待（阻塞）
      // 注意：这可能导致死锁，如果协程永远不完成
      handle_.promise().completion_future.wait();
    }
    // 销毁协程帧
    handle_.destroy();
    handle_ = {};
  }

  /// @brief 关联的协程句柄
  std::coroutine_handle<promise_type> handle_;
};

/// @brief void 类型特化，用于无返回值的协程
///
/// 与通用模板的主要区别：
/// - 使用 return_void() 代替 return_value()
/// - await_resume() 不返回值
template <>
class Task<void> {
 public:
  struct promise_type {
    std::promise<void> completion_promise;
    std::shared_future<void> completion_future;
    std::coroutine_handle<> continuation;
    std::exception_ptr unhandled_error;
    bool completion_set{false};

    promise_type()
        : completion_future(completion_promise.get_future().share()) {}

    Task get_return_object() {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_never initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }

      std::coroutine_handle<> await_suspend(
          std::coroutine_handle<promise_type> handle) noexcept {
        auto& promise = handle.promise();
        promise.PublishCompletion();

        auto continuation = promise.continuation;
        // 对称转移：避免在当前协程 final_suspend 栈帧内直接 resume continuation
        // 造成重入销毁协程帧的未定义行为。
        if (continuation) {
          return continuation;
        }
        return std::noop_coroutine();
      }

      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    /// @brief 处理 co_return; 或协程体执行完毕
    void return_void() {}

    void unhandled_exception() {
      unhandled_error = std::current_exception();
    }

    void PublishCompletion() noexcept {
      if (completion_set) {
        return;
      }
      completion_set = true;

      if (unhandled_error != nullptr) {
        completion_promise.set_exception(unhandled_error);
        return;
      }

      completion_promise.set_value();
    }
  };

  Task() = delete;

  explicit Task(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  Task(Task&& other) noexcept : handle_(other.handle_) { other.handle_ = {}; }

  Task& operator=(Task&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    Reset();
    handle_ = other.handle_;
    other.handle_ = {};
    return *this;
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  ~Task() { Reset(); }

  class Awaiter {
   public:
    explicit Awaiter(std::coroutine_handle<promise_type> handle)
        : handle_(handle) {}

    bool await_ready() const {
      return handle_.promise().completion_future.wait_for(
                 std::chrono::seconds(0)) == std::future_status::ready;
    }

    bool await_suspend(std::coroutine_handle<> awaiting) {
      auto& promise = handle_.promise();
      promise.continuation = awaiting;
      return promise.completion_future.wait_for(std::chrono::seconds(0)) !=
             std::future_status::ready;
    }

    /// @brief 恢复时无返回值
    void await_resume() { handle_.promise().completion_future.get(); }

   private:
    std::coroutine_handle<promise_type> handle_;
  };

  auto operator co_await() & { return Awaiter{handle_}; }
  auto operator co_await() && { return Awaiter{handle_}; }

  /// @brief 同步等待 void Task 完成
  void Get() { handle_.promise().completion_future.get(); }

 private:
  /// @brief 重置 Task，销毁关联的协程
  ///
  /// 在销毁协程前，检查协程是否已执行完毕。
  void Reset() {
    if (!handle_) {
      return;
    }

    // 检查协程是否已完成
    if (handle_.promise().completion_future.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      // 协程未完成，等待（阻塞）
      handle_.promise().completion_future.wait();
    }
    // 销毁协程帧
    handle_.destroy();
    handle_ = {};
  }

  std::coroutine_handle<promise_type> handle_;
};

/// @brief 同步等待 Task 完成的辅助函数
/// @tparam T Task 的返回值类型
/// @param task 要等待的 Task
/// @return Task 的返回值
///
/// 提供统一的同步等待接口，适用于需要在非协程上下文中启动协程的场景。
template <typename T>
T SyncWait(Task<T> task) {
  return task.Get();
}

/// @brief void Task 的同步等待重载
/// @param task 要等待的 void Task
inline void SyncWait(Task<void> task) { task.Get(); }

}  // namespace rpc::coroutine