// thread_pool.cpp
// 线程池实现文件（基于 MpscRingQueue 优化）

#include "common/thread_pool.h"

#include <exception>
#include <span>
#include <string>
#include <utility>

namespace rpc::common {

// ============================================================================
// 构造与析构
// ============================================================================

ThreadPool::ThreadPool(std::size_t thread_count)
    : thread_count_(thread_count == 0 ? 1 : thread_count) {}

ThreadPool::~ThreadPool() {
  Stop();
  Join();
}

// ============================================================================
// 公共方法
// ============================================================================

bool ThreadPool::Start(std::string* error_msg) {
  if (!worker_data_.empty()) {
    if (error_msg != nullptr) {
      *error_msg = "thread pool already started";
    }
    return false;
  }

  stop_requested_.store(false, std::memory_order_relaxed);
  accepting_tasks_.store(true, std::memory_order_relaxed);

  worker_data_.reserve(thread_count_);
  for (std::size_t i = 0; i < thread_count_; ++i) {
    worker_data_.push_back(std::make_unique<WorkerData>());
  }

  try {
    workers_.reserve(thread_count_);
    for (std::size_t i = 0; i < thread_count_; ++i) {
      workers_.emplace_back([this, i]() { WorkerMain(i); });
    }
  } catch (const std::exception& ex) {
    accepting_tasks_.store(false, std::memory_order_relaxed);
    stop_requested_.store(true, std::memory_order_relaxed);

    for (auto& data : worker_data_) {
      {
        std::lock_guard<std::mutex> lock(data->cv_mutex);
      }
      data->cv.notify_all();
    }

    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }

    worker_data_.clear();
    workers_.clear();

    if (error_msg != nullptr) {
      *error_msg = std::string("failed to start thread pool: ") + ex.what();
    }
    return false;
  }

  return true;
}

bool ThreadPool::Submit(std::function<void()> task) {
  if (!task) {
    return false;
  }

  if (!accepting_tasks_.load(std::memory_order_relaxed)) {
    return false;
  }

  // thread_local 计数器：消除多线程竞争 next_worker_ 原子变量的开销
  thread_local std::size_t local_index = 0;
  std::size_t idx = local_index % thread_count_;
  ++local_index;

  // 尝试提交到选中的 worker 队列，若满则 fallback 到下一个
  for (std::size_t attempt = 0; attempt < thread_count_; ++attempt) {
    WorkerData& data = *worker_data_[idx];

    if (data.queue.TryPush(std::move(task))) {
      data.pending.fetch_add(1, std::memory_order_relaxed);
      submitted_tasks_.fetch_add(1, std::memory_order_relaxed);

      // 仅在 worker 处于等待状态时才做 cv 通知，避免无谓的系统调用
      if (data.is_waiting.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(data.cv_mutex);
        data.cv.notify_one();
      }
      return true;
    }

    idx = (idx + 1) % thread_count_;
  }

  // 慢路径：所有工作线程队列已满，阻塞等待容量释放
  // 日志场景可以丢弃，线程池场景必须确保任务不丢
  {
    WorkerData& data = *worker_data_[idx];
    std::unique_lock<std::mutex> lock(data.cv_mutex);
    while (!data.queue.TryPush(std::move(task))) {
      if (!accepting_tasks_.load(std::memory_order_relaxed)) {
        return false;
      }
      data.cv.wait(lock);
    }
    data.pending.fetch_add(1, std::memory_order_relaxed);
    submitted_tasks_.fetch_add(1, std::memory_order_relaxed);
    data.cv.notify_one();
  }
  return true;
}

void ThreadPool::Stop() {
  const bool was_accepting = accepting_tasks_.exchange(false);
  const bool was_stopping = stop_requested_.exchange(true);
  if (was_accepting || !was_stopping) {
    for (auto& data : worker_data_) {
      {
        std::lock_guard<std::mutex> lock(data->cv_mutex);
      }
      data->cv.notify_all();
    }
  }
}

void ThreadPool::Join() {
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  worker_data_.clear();
}

bool ThreadPool::IsRunning() const noexcept {
  return accepting_tasks_.load(std::memory_order_relaxed);
}

StatsSnapshot ThreadPool::GetStatsSnapshot() const {
  StatsSnapshot snapshot;
  snapshot.thread_count = thread_count_;
  snapshot.active_workers = active_workers_.load(std::memory_order_relaxed);
  snapshot.submitted_tasks = submitted_tasks_.load(std::memory_order_relaxed);
  snapshot.completed_tasks = completed_tasks_.load(std::memory_order_relaxed);
  snapshot.accepting_tasks = accepting_tasks_.load(std::memory_order_relaxed);

  // 平均每个 worker 的任务数（使用原子计数值，避免 data race）
  std::size_t total_queue_size = 0;
  for (const auto& data : worker_data_) {
    total_queue_size += data->pending.load(std::memory_order_relaxed);
  }
  snapshot.queue_size = total_queue_size;

  return snapshot;
}

// ============================================================================
// 私有方法
// ============================================================================

void ThreadPool::WorkerMain(std::size_t worker_index) {
  WorkerData& data = *worker_data_[worker_index];

  // 批量取出任务的预分配缓冲区（栈上分配，零堆开销）
  std::array<std::function<void()>, kBatchSize> batch;

  while (true) {
    // ===== 批量取出任务 =====
    std::size_t count =
        data.queue.PopBatch(std::span(batch.data(), batch.size()));

    if (count == 0) {
      // 队列空，准备睡眠
      if (stop_requested_.load(std::memory_order_relaxed)) {
        return;
      }

      // 设置等待标志（release 语义确保 in-queue 检查可见）
      data.is_waiting.store(true, std::memory_order_release);

      // 双重检查：防止在设置标志和获取锁之间漏任务
      if (!data.queue.Empty()) {
        data.is_waiting.store(false, std::memory_order_relaxed);
        continue;  // 有任务到达，回去处理
      }

      if (stop_requested_.load(std::memory_order_relaxed)) {
        data.is_waiting.store(false, std::memory_order_relaxed);
        return;
      }

      std::unique_lock<std::mutex> lock(data.cv_mutex);
      data.cv.wait(lock, [&]() {
        return stop_requested_.load(std::memory_order_relaxed) ||
               !data.queue.Empty();
      });
      data.is_waiting.store(false, std::memory_order_relaxed);
      continue;
    }

    // ===== 批量执行任务 =====
    // 通知阻塞的生产者：队列已释放容量
    data.cv.notify_all();

    // 更新队列计数
    data.pending.fetch_sub(count, std::memory_order_relaxed);

    // 当前 worker 进入活跃状态（语义："正在工作的线程数"，而非任务数）
    active_workers_.fetch_add(1, std::memory_order_relaxed);

    for (std::size_t i = 0; i < count; ++i) {
      try {
        if (batch[i]) {
          batch[i]();
        }
      } catch (...) {
        // 任务异常不应终止工作线程
      }

      batch[i] = nullptr;  // 尽早释放捕获的资源
    }

    active_workers_.fetch_sub(1, std::memory_order_relaxed);
    completed_tasks_.fetch_add(count, std::memory_order_relaxed);
  }
}

}  // namespace rpc::common
