// thread_pool.cpp
// 线程池实现文件

/**
 * @file thread_pool.cpp
 * @brief 固定线程数的最小线程池实现
 *
 * ## 简易使用示例
 *
 * ### 基础用法
 * @code
 *   #include "common/thread_pool.h"
 *
 *   // 1. 创建线程池（4个工作线程）
 *   rpc::common::ThreadPool pool(4);
 *
 *   // 2. 启动线程池
 *   if (!pool.Start()) {
 *     // 启动失败处理
 *     return;
 *   }
 *
 *   // 3. 提交任务
 *   pool.Submit([]() {
 *     std::cout << "Hello from thread pool!" << std::endl;
 *   });
 *
 *   // 4. 提交带参数的任务
 *   int x = 10;
 *   pool.Submit([x]() {
 *     std::cout << "x = " << x << std::endl;
 *   });
 *
 *   // 5. 析构时自动 Stop() + Join()
 *   // 离开作用域，线程池会自动关闭并等待所有线程结束
 * @endcode
 *
 * ### 在 RPC Server 中的使用
 * @code
 *   // 创建业务线程池
 *   ThreadPool business_pool(8);
 *   business_pool.Start();
 *
 *   // 在 handler 中提交耗时任务到线程池
 *   void HandleRequest(Request req) {
 *     // 将耗时操作提交到线程池
 *     business_pool.Submit([req]() {
 *       ProcessRequest(req);  // 在工作线程中执行
 *     });
 *   }
 * @endcode
 *
 * ### 获取统计信息
 * @code
 *   auto stats = pool.GetStatsSnapshot();
 *   std::cout << "线程数: " << stats.thread_count << std::endl;
 *   std::cout << "活跃线程: " << stats.active_workers << std::endl;
 *   std::cout << "队列大小: " << stats.queue_size << std::endl;
 *   std::cout << "已提交: " << stats.submitted_tasks << std::endl;
 *   std::cout << "已完成: " << stats.completed_tasks << std::endl;
 * @endcode
 *
 * ### 手动控制生命周期
 * @code
 *   ThreadPool pool(4);
 *   pool.Start();
 *
 *   // ... 提交任务 ...
 *
 *   // 手动停止（可选，析构时会自动调用）
 *   pool.Stop();   // 停止接受新任务，通知工作线程退出
 *   pool.Join();   // 等待所有工作线程结束
 * @endcode
 *
 * ## 注意事项
 *
 * 1. 任务通过值捕获，注意生命周期问题
 * 2. 任务异常会被捕获，不会终止工作线程
 * 3. Stop() 后 Submit() 会返回 false
 * 4. 析构时自动调用 Stop() + Join()
 */

#include "common/thread_pool.h"

#include <exception>
#include <utility>

namespace rpc::common {

// ============================================================================
// 构造与析构
// ============================================================================

// 构造函数实现
// 初始化线程数量，确保至少有一个线程
// 线程的实际创建在 Start() 方法中进行
ThreadPool::ThreadPool(std::size_t thread_count)
    : thread_count_(thread_count == 0 ? 1 : thread_count) {}

// 析构函数实现
// 确保线程池在销毁时正确关闭：
// 1. 调用 Stop() 停止接受新任务并通知工作线程
// 2. 调用 Join() 等待所有工作线程退出
ThreadPool::~ThreadPool() {
  Stop();
  Join();
}

// ============================================================================
// 公共方法
// ============================================================================

// 启动线程池
// 实现细节：
// 1. 检查是否已经启动（workers_ 非空）
// 2. 重置停止标志，设置接受任务标志
// 3. 创建指定数量的工作线程
// 异常安全：如果线程创建失败，回滚状态并返回错误信息
bool ThreadPool::Start(std::string* error_msg) {
  {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否已经启动
    if (!workers_.empty()) {
      if (error_msg != nullptr) {
        *error_msg = "thread pool already started";
      }
      return false;
    }

    // 重置状态标志
    stop_requested_.store(false);
    accepting_tasks_.store(true);
  }

  // 在锁外创建线程，避免长时间持锁影响 Submit/Stop。
  std::vector<std::thread> started_workers;
  try {
    started_workers.reserve(thread_count_);
    for (std::size_t i = 0; i < thread_count_; ++i) {
      // 每个工作线程执行 WorkerMain() 函数
      started_workers.emplace_back([this]() { WorkerMain(); });
    }
  } catch (const std::exception& ex) {
    // 创建失败，回滚状态并回收已成功创建的线程。
    accepting_tasks_.store(false);
    stop_requested_.store(true);
    cv_.notify_all();

    for (auto& worker : started_workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }

    if (error_msg != nullptr) {
      *error_msg = std::string("failed to start thread pool: ") + ex.what();
    }
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    workers_.swap(started_workers);
  }

  return true;
}

// 提交任务到线程池
// 实现细节：
// 1. 检查任务是否有效
// 2. 加锁检查是否接受任务
// 3. 将任务加入队列尾部
// 4. 递增提交计数
// 5. 通知一个等待的工作线程
// 注意: 使用 notify_one() 而非 notify_all() 以减少不必要的唤醒
bool ThreadPool::Submit(std::function<void()> task) {
  // 拒绝空任务
  if (!task) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);

    // 检查是否仍在接受任务
    if (!accepting_tasks_.load()) {
      return false;
    }

    // 将任务加入队列
    tasks_.push_back(std::move(task));
    submitted_tasks_.fetch_add(1);
  }

  // 唤醒一个工作线程
  cv_.notify_one();
  return true;
}

// 停止线程池
// 实现细节：
// 1. 设置 accepting_tasks_ 为 false，拒绝新任务
// 2. 设置 stop_requested_ 为 true，通知工作线程退出
// 3. 唤醒所有等待的工作线程
// 注意: 使用原子操作，无需加锁，可安全地从任意线程调用
void ThreadPool::Stop() {
  const bool was_accepting = accepting_tasks_.exchange(false);
  const bool was_stopping = stop_requested_.exchange(true);
  if (was_accepting || !was_stopping) {
    cv_.notify_all();  // 唤醒所有等待的工作线程
  }
}

// 等待所有工作线程结束
// 实现细节：
// 1. 从 workers_ 容器中取出所有线程句柄
// 2. 逐个调用 join() 等待线程结束
// 注意: 使用 swap 技巧在锁外执行 join()，避免死锁
// 注意: join() 可能阻塞，不应在持有锁时调用
void ThreadPool::Join() {
  std::vector<std::thread> to_join;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    to_join.swap(workers_);  // 取出所有线程句柄，清空 workers_
  }

  // 在锁外执行 join，避免死锁
  for (auto& worker : to_join) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

// 检查线程池是否正在运行
// 注意: 使用原子变量，无锁实现，高效且线程安全
bool ThreadPool::IsRunning() const noexcept { return accepting_tasks_.load(); }

// 获取线程池统计信息快照
// 收集所有统计数据，提供一致的视图
// 需要加锁获取队列大小，其他数据使用原子变量
StatsSnapshot ThreadPool::GetStatsSnapshot() const {
  StatsSnapshot snapshot;

  // 获取原子变量的值
  snapshot.thread_count = thread_count_;
  snapshot.active_workers = active_workers_.load();
  snapshot.submitted_tasks = submitted_tasks_.load();
  snapshot.completed_tasks = completed_tasks_.load();
  snapshot.accepting_tasks = accepting_tasks_.load();

  // 获取队列大小需要加锁
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.queue_size = tasks_.size();
  }

  return snapshot;
}

// ============================================================================
// 私有方法
// ============================================================================

// 工作线程主函数
//
// 工作线程的生命周期：
//   1. 等待条件变量 (有任务或停止信号)
//   2. 检查退出条件：停止信号 && 队列为空
//   3. 从队列取出任务，更新 active_workers_ 计数
//   4. 执行任务 (在锁外执行)
//   5. 更新 active_workers_ 和 completed_tasks_ 计数
//   6. 循环回到等待
//
// 关键点：
// - 使用条件变量等待，避免忙等待
// - 任务执行在锁外进行，最大化并发性
// - 统计计数器使用原子操作，避免锁开销
void ThreadPool::WorkerMain() {
  while (true) {
    std::function<void()> task;

    // ===== 获取任务阶段 =====
    {
      std::unique_lock<std::mutex> lock(mutex_);

      // 等待条件：有任务可执行，或者收到停止信号
      cv_.wait(lock,
               [this]() { return stop_requested_.load() || !tasks_.empty(); });

      // 检查退出条件：停止信号 && 队列为空
      if (tasks_.empty()) {
        if (stop_requested_.load()) {
          return;  // 退出工作线程
        }
        continue;  // 虚假唤醒，继续等待
      }

      // 从队列头部取出任务
      task = std::move(tasks_.front());
      tasks_.pop_front();

      // 更新活跃工作线程计数
      active_workers_.fetch_add(1);
    }

    // ===== 执行任务阶段 =====
    // 在锁外执行任务，允许其他线程操作队列
    try {
      task();
    } catch (...) {
      // 任务异常不应终止工作线程；继续处理后续任务。
    }

    // ===== 更新统计阶段 =====
    active_workers_.fetch_sub(1);
    completed_tasks_.fetch_add(1);
  }
}

}  // namespace rpc::common