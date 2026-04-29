// thread_pool_benchmark.cpp
// 纯 ThreadPool 性能微基准测试
//
// 内嵌旧版（mutex + deque）和新版（per-worker MpscRingQueue）对比

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "common/log.h"
#include "common/thread_pool.h"

// ============================================================================
// Old ThreadPool (mutex + deque, 优化前版本)
// ============================================================================
class OldThreadPool {
 public:
  explicit OldThreadPool(std::size_t n) : count_(n == 0 ? 1 : n) {}
  ~OldThreadPool() {
    Stop();
    Join();
  }
  bool Start(std::string* = nullptr) {
    accepting_.store(true);
    stop_.store(false);
    workers_.reserve(count_);
    for (std::size_t i = 0; i < count_; ++i) {
      workers_.emplace_back([this] { Worker(); });
    }
    return true;
  }
  bool Submit(std::function<void()> task) {
    if (!task) return false;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (!accepting_.load()) return false;
      tasks_.push_back(std::move(task));
      submitted_.fetch_add(1);
    }
    cv_.notify_one();
    return true;
  }
  void Stop() {
    accepting_.store(false);
    stop_.store(true);
    cv_.notify_all();
  }
  void Join() {
    for (auto& w : workers_) {
      if (w.joinable()) w.join();
    }
    workers_.clear();
  }

 private:
  void Worker() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return stop_.load() || !tasks_.empty(); });
        if (tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
        active_.fetch_add(1);
      }
      try { task(); } catch (...) {}
      active_.fetch_sub(1);
      completed_.fetch_add(1);
    }
  }
  std::size_t count_;
  std::vector<std::thread> workers_;
  std::deque<std::function<void()>> tasks_;
  std::mutex mtx_;
  std::condition_variable cv_;
  std::atomic<bool> accepting_{false};
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> submitted_{0};
  std::atomic<std::size_t> active_{0};
  std::atomic<std::size_t> completed_{0};
};

// ============================================================================
// Benchmark helpers
// ============================================================================

struct Result {
  std::string label;
  double qps;
  double avg_us;
  double p50_us;
  double p95_us;
  double p99_us;
  std::size_t tasks;
};

void Print(const Result& r) {
  std::cout << "[" << r.label << "]\n"
            << "  QPS=" << std::fixed << std::setprecision(0) << r.qps
            << " tasks/s"
            << "  lat(us): avg=" << std::setprecision(0) << r.avg_us
            << " p50=" << r.p50_us << " p95=" << r.p95_us << " p99="
            << r.p99_us << "\n";
}

// 通用测试：多个 producer 提交任务，自旋等待提交成功
template <typename Pool>
Result RunBench(const std::string& label, int tasks, int workers,
                int producers, int sleep_us) {
  if (workers == 0) workers = 1;

  Pool pool(static_cast<std::size_t>(workers));
  pool.Start(nullptr);

  std::atomic<std::size_t> executed{0};
  auto latencies = std::make_unique<std::atomic<int64_t>[]>(
      static_cast<std::size_t>(tasks));

  auto begin = std::chrono::steady_clock::now();

  std::vector<std::thread> pts;
  pts.reserve(static_cast<std::size_t>(producers));
  int per = tasks / producers;
  int rem = tasks % producers;

  for (int p = 0; p < producers; ++p) {
    int cnt = per + (p < rem ? 1 : 0);
    int offset = p * per + std::min(p, rem);
    pts.emplace_back([&pool, &executed, &latencies, cnt, offset, sleep_us]() {
      for (int i = 0; i < cnt; ++i) {
        int idx = offset + i;
        auto t1 = std::chrono::steady_clock::now();
        while (!pool.Submit([&latencies, idx, t1, sleep_us]() {
          auto t2 = std::chrono::steady_clock::now();
          if (sleep_us > 0) {
            auto target = t2 + std::chrono::microseconds(sleep_us);
            while (std::chrono::steady_clock::now() < target) {
            }
          }
          latencies[static_cast<std::size_t>(idx)].store(
              std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
                  .count());
        })) {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& t : pts) t.join();
  pool.Stop();
  pool.Join();

  auto end = std::chrono::steady_clock::now();
  double dur = std::chrono::duration<double>(end - begin).count();

  // 收集延迟
  std::vector<int64_t> lats;
  lats.reserve(static_cast<std::size_t>(tasks));
  for (int i = 0; i < tasks; ++i) {
    lats.push_back(latencies[i].load());
  }
  std::sort(lats.begin(), lats.end());
  auto sz = lats.size();
  int64_t sum = 0;
  for (auto v : lats) sum += v;
  double avg = static_cast<double>(sum) / static_cast<double>(sz);

  return {
      .label = label,
      .qps = static_cast<double>(sz) / dur,
      .avg_us = avg,
      .p50_us = static_cast<double>(lats[sz / 2]),
      .p95_us = static_cast<double>(
          lats[static_cast<std::size_t>(static_cast<double>(sz) * 0.95)]),
      .p99_us = static_cast<double>(
          lats[static_cast<std::size_t>(static_cast<double>(sz) * 0.99)]),
      .tasks = sz,
  };
}

int main(int argc, char** argv) {
  rpc::common::SetLogLevel(rpc::common::LogLevel::kError);

  int nt = (argc > 1) ? std::max(1000, std::atoi(argv[1])) : 20000;
  int nw = (argc > 2) ? std::max(1, std::atoi(argv[2])) : 4;
  int np = (argc > 3) ? std::max(1, std::atoi(argv[3])) : 4;

  std::cout << "=== ThreadPool Before/After Benchmark ===\n";
  std::cout << "tasks=" << nt << " workers=" << nw << " producers=" << np
            << "\n\n";

  // --- Scenario 1: empty task, high throughput ---
  std::cout << "--- Scenario: empty task (pure dispatch overhead) ---\n";
  auto old1 = RunBench<OldThreadPool>("OLD  deque+mutex          ", nt, nw, np,
                                      0);
  auto new1 = RunBench<rpc::common::ThreadPool>("NEW  per-worker MpscRingQueue",
                                                nt, nw, np, 0);
  Print(old1);
  Print(new1);
  std::cout << "  SPEEDUP: " << std::fixed << std::setprecision(1)
            << new1.qps / old1.qps << "x\n\n";

  // --- Scenario 2: 10us busy-wait task ---
  std::cout << "--- Scenario: 10us busy-wait task ---\n";
  auto old2 = RunBench<OldThreadPool>("OLD  deque+mutex          ", nt, nw, np,
                                      10);
  auto new2 = RunBench<rpc::common::ThreadPool>("NEW  per-worker MpscRingQueue",
                                                nt, nw, np, 10);
  Print(old2);
  Print(new2);
  std::cout << "  SPEEDUP: " << std::fixed << std::setprecision(1)
            << new2.qps / old2.qps << "x\n\n";

  // --- Scenario 3: high contention (many producers) ---
  std::cout << "--- Scenario: high concurrency (" << np * 4
            << " producers) ---\n";
  auto old3 = RunBench<OldThreadPool>("OLD  deque+mutex          ", nt, nw,
                                      np * 4, 0);
  auto new3 = RunBench<rpc::common::ThreadPool>(
      "NEW  per-worker MpscRingQueue", nt, nw, np * 4, 0);
  Print(old3);
  Print(new3);
  std::cout << "  SPEEDUP: " << std::fixed << std::setprecision(1)
            << new3.qps / old3.qps << "x\n\n";

  // --- Scenario 4: scaling with worker count ---
  std::cout << "--- Scenario: worker count scaling (1-8 workers) ---\n";
  for (int w : {1, 2, 4, 8}) {
    auto old_s =
        RunBench<OldThreadPool>("OLD  " + std::to_string(w) + "w", nt / 2, w,
                                w, 0);
    auto new_s =
        RunBench<rpc::common::ThreadPool>("NEW  " + std::to_string(w) + "w",
                                          nt / 2, w, w, 0);
    Print(old_s);
    Print(new_s);
    std::cout << "  SPEEDUP: " << std::fixed << std::setprecision(1)
              << new_s.qps / old_s.qps << "x\n";
  }

  std::cout << "\nDone.\n";
  return 0;
}
