// thread_pool.h
// 线程池实现
//
// 提供一个高效、线程安全的线程池，用于异步执行任务。
// 支持动态任务提交、优雅关闭、统计信息查询等功能。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace rpc::common {

// 线程池统计信息的快照
// 用于获取线程池当前状态的原子快照，便于监控和调试
struct StatsSnapshot {
  std::size_t thread_count{0};     // 线程池中的线程总数
  std::size_t active_workers{0};   // 当前正在执行任务的工作线程数
  std::size_t queue_size{0};       // 等待执行的任务队列长度
  std::size_t submitted_tasks{0};  // 累计提交的任务总数
  std::size_t completed_tasks{0};  // 累计完成的任务总数
  bool accepting_tasks{false};     // 是否正在接受新任务
};

// 线程安全的任务队列线程池
//
// 该类实现了一个经典的线程池模式：
// - 固定数量的工作线程
// - FIFO 任务队列（使用双端队列实现）
// - 条件变量进行线程同步
// - 支持优雅关闭和统计信息查询
//
// 线程安全保证：
// - 所有公共方法都是线程安全的
// - 内部使用 mutex 保护共享状态
// - 使用 atomic 变量进行统计计数
class ThreadPool {
 public:
  // 构造函数
  // 参数 thread_count: 工作线程数量，若为 0 则默认使用 1 个线程
  explicit ThreadPool(std::size_t thread_count);

  // 析构函数
  // 自动调用 Stop() 和 Join() 确保线程正确终止
  ~ThreadPool();

  // 禁止拷贝和移动
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  // 启动线程池
  // 参数 error_msg: 错误信息输出参数，可为 nullptr
  // 返回值: 启动成功返回 true，失败返回 false
  // 说明: 创建指定数量的工作线程并开始处理任务
  bool Start(std::string* error_msg);

  // 提交任务到线程池
  // 参数 task: 要执行的任务，是一个无参数无返回值的可调用对象
  // 返回值: 提交成功返回 true，失败返回 false
  // 说明: 将任务加入队列，唤醒一个等待的工作线程执行
  bool Submit(std::function<void()> task);

  // 通用任务提交接口（可变参模板）
  // 参数 fn/args: 任意可调用对象及其参数
  // 返回值: 与任务返回类型匹配的 future；若提交失败，返回 invalid future
  // 说明: 内部通过 packaged_task 包装，再复用基础 Submit() 入队
  // template <typename Fn, typename... Args>
  // auto SubmitFuture(Fn&& fn, Args&&... args) -> std::future<
  //     std::invoke_result_t<std::decay_t<Fn>, std::decay_t<Args>...>> {
  //   using ReturnT =
  //       std::invoke_result_t<std::decay_t<Fn>, std::decay_t<Args>...>;

  //   auto task = std::make_shared<std::packaged_task<ReturnT()>>(
  //       std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...));

  //   std::future<ReturnT> future = task->get_future();
  //   const bool submitted = Submit([task]() { (*task)(); });
  //   if (!submitted) {
  //     return std::future<ReturnT>{};
  //   }
  //   return future;
  // }

  // 停止接受新任务并通知工作线程
  // 说明: 设置停止标志，唤醒所有工作线程，工作线程将完成当前任务后退出
  void Stop();

  // 等待所有工作线程结束
  // 说明: 阻塞等待所有工作线程执行完毕，必须先调用 Stop() 才能确保线程退出
  void Join();

  // 检查线程池是否正在运行
  // 返回值: 若线程池正在接受任务返回 true，否则返回 false
  [[nodiscard]] bool IsRunning() const noexcept;

  // 获取线程池统计信息快照
  // 返回值: 包含当前各项统计数据的快照对象
  [[nodiscard]] StatsSnapshot GetStatsSnapshot() const;

 private:
  // 工作线程的主函数
  // 工作线程循环执行：等待任务 -> 取出任务 -> 执行任务 -> 更新统计
  void WorkerMain();

  // ==================== 成员变量 ====================

  std::size_t thread_count_;  // 工作线程数量

  // 同步原语
  mutable std::mutex mutex_;    // 保护 tasks_ 和 workers_ 的互斥锁
  std::condition_variable cv_;  // 用于通知工作线程的条件变量

  // 任务队列
  std::deque<std::function<void()>> tasks_;  // FIFO 任务队列

  // 工作线程
  std::vector<std::thread> workers_;  // 工作线程容器

  // 原子状态标志
  std::atomic<bool> accepting_tasks_{false};  // 是否接受新任务
  std::atomic<bool> stop_requested_{false};   // 是否请求停止

  // 原子统计计数器
  std::atomic<std::size_t> active_workers_{0};   // 当前活跃的工作线程数
  std::atomic<std::size_t> submitted_tasks_{0};  // 累计提交的任务数
  std::atomic<std::size_t> completed_tasks_{0};  // 累计完成的任务数
};

}  // namespace rpc::common