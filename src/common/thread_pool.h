// thread_pool.h
// 线程池实现（使用 MpscRingQueue 优化）
//
// 提供一个高效、线程安全的线程池，用于异步执行任务。
// 使用每 worker 一个 MpscRingQueue 的设计，消除 Submit 路径的 mutex 竞争。
// 支持动态任务提交、优雅关闭、统计信息查询等功能。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/mpsc_ring_queue.h"

namespace rpc::common {

using detail::MpscRingQueue;

// 线程池统计信息的快照
struct StatsSnapshot {
  std::size_t thread_count{0};
  std::size_t active_workers{0};
  std::size_t queue_size{0};
  std::size_t submitted_tasks{0};
  std::size_t completed_tasks{0};
  bool accepting_tasks{false};
};

// 线程安全的任务队列线程池（基于 MpscRingQueue 优化）
//
// 设计要点：
// - 每个 worker 拥有独立的 MpscRingQueue，消除 Submit 路径的 mutex 竞争
// - Submit 使用 thread_local counter 进行 round-robin 分配，消除原子竞争
// - is_waiting 标志避免无谓的 cv 唤醒，减少系统调用
// - PopBatch 批量出队 + 批量更新统计，降低原子操作频率
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t thread_count);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  bool Start(std::string* error_msg);
  bool Submit(std::function<void()> task);
  void Stop();
  void Join();

  [[nodiscard]] bool IsRunning() const noexcept;
  [[nodiscard]] StatsSnapshot GetStatsSnapshot() const;

 private:
  void WorkerMain(std::size_t worker_index);

  static constexpr std::size_t kQueueCapacity = 1024;
  static constexpr std::size_t kBatchSize = 32;

  // 每个 worker 的数据（队列 + 同步原语）
  struct WorkerData {
    MpscRingQueue<std::function<void()>, kQueueCapacity> queue;
    std::mutex cv_mutex;
    std::condition_variable cv;
    std::atomic<bool> is_waiting{false};  // worker 是否即将/正在 cv.wait
    std::atomic<std::size_t> pending{0};  // 队列中待执行任务数（原子近似值）
  };

  std::size_t thread_count_;

  // 每 worker 数据（unique_ptr 因为 WorkerData 不可移动）
  std::vector<std::unique_ptr<WorkerData>> worker_data_;

  // 工作线程
  std::vector<std::thread> workers_;

  // 原子状态标志
  std::atomic<bool> accepting_tasks_{false};
  std::atomic<bool> stop_requested_{false};

  // 原子统计计数器
  alignas(64) std::atomic<std::size_t> active_workers_{0};
  alignas(64) std::atomic<std::size_t> submitted_tasks_{0};
  alignas(64) std::atomic<std::size_t> completed_tasks_{0};
};

}  // namespace rpc::common
