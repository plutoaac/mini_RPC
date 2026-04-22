#include "common/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct BenchmarkConfig {
  int threads{4};
  int messages_per_thread{500000};
  int message_bytes{128};
  int flush_ms{1000};
  int sample_stride{64};
  std::string log_file{"./benchmarks/results/async_logger_benchmark.log"};
};

struct ThreadStats {
  std::vector<std::int64_t> sampled_submit_ns;
};

[[nodiscard]] std::string BuildMessage(int bytes, int worker_id) {
  std::string message;
  message.reserve(static_cast<std::size_t>(std::max(32, bytes)));
  message = "logger-bench worker=" + std::to_string(worker_id) + " payload=";

  while (static_cast<int>(message.size()) < bytes) {
    message += "abcdefghijklmnopqrstuvwxyz0123456789";
  }
  message.resize(static_cast<std::size_t>(bytes));
  return message;
}

[[nodiscard]] bool ParsePositiveInt(std::string_view arg,
                                    std::string_view prefix,
                                    int* out) {
  if (!arg.starts_with(prefix)) {
    return false;
  }
  const std::string value(arg.substr(prefix.size()));
  try {
    const int parsed = std::stoi(value);
    if (parsed <= 0) {
      return false;
    }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] bool ParseArgs(int argc, char** argv, BenchmarkConfig* config) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    int parsed = 0;
    if (ParsePositiveInt(arg, "--threads=", &parsed)) {
      config->threads = parsed;
      continue;
    }
    if (ParsePositiveInt(arg, "--messages-per-thread=", &parsed)) {
      config->messages_per_thread = parsed;
      continue;
    }
    if (ParsePositiveInt(arg, "--message-bytes=", &parsed)) {
      config->message_bytes = parsed;
      continue;
    }
    if (ParsePositiveInt(arg, "--flush-ms=", &parsed)) {
      config->flush_ms = parsed;
      continue;
    }
    if (ParsePositiveInt(arg, "--sample-stride=", &parsed)) {
      config->sample_stride = parsed;
      continue;
    }
    if (arg.starts_with("--log-file=")) {
      config->log_file = std::string(arg.substr(std::string_view("--log-file=").size()));
      continue;
    }
    if (arg == "--help") {
      std::cout
          << "Usage: async_logger_benchmark [options]\n"
          << "  --threads=N\n"
          << "  --messages-per-thread=N\n"
          << "  --message-bytes=N\n"
          << "  --flush-ms=N\n"
          << "  --sample-stride=N\n"
          << "  --log-file=PATH\n";
      return false;
    }

    std::cerr << "unknown arg: " << arg << '\n';
    return false;
  }
  return true;
}

[[nodiscard]] double Percentile(std::vector<std::int64_t> sorted, double p) {
  if (sorted.empty()) {
    return 0.0;
  }
  std::sort(sorted.begin(), sorted.end());
  const double idx = p * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(idx);
  const std::size_t hi = std::min(sorted.size() - 1, lo + 1);
  const double frac = idx - static_cast<double>(lo);
  return static_cast<double>(sorted[lo]) * (1.0 - frac) +
         static_cast<double>(sorted[hi]) * frac;
}

}  // namespace

int main(int argc, char** argv) {
  BenchmarkConfig config;
  if (!ParseArgs(argc, argv, &config)) {
    return 1;
  }

  namespace fs = std::filesystem;
  if (!config.log_file.empty()) {
    std::error_code ec;
    const fs::path path(config.log_file);
    if (path.has_parent_path()) {
      fs::create_directories(path.parent_path(), ec);
    }
    fs::remove(path, ec);
  }

  rpc::common::LoggerOptions options;
  options.file_path = config.log_file;
  options.min_level = rpc::common::LogLevel::kInfo;
  options.flush_interval = std::chrono::milliseconds(config.flush_ms);
  rpc::common::InitLogger(options);

  std::atomic<int> ready{0};
  std::atomic<bool> go{false};

  std::vector<ThreadStats> stats(static_cast<std::size_t>(config.threads));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(config.threads));

  for (int t = 0; t < config.threads; ++t) {
    workers.emplace_back([&, t] {
      ThreadStats local;
      local.sampled_submit_ns.reserve(static_cast<std::size_t>(
          std::max(1, config.messages_per_thread / config.sample_stride)));

      const std::string payload = BuildMessage(config.message_bytes, t);

      ready.fetch_add(1, std::memory_order_acq_rel);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (int i = 0; i < config.messages_per_thread; ++i) {
        if ((i % config.sample_stride) == 0) {
          const auto begin = Clock::now();
          rpc::common::LogInfo(payload);
          const auto end = Clock::now();
          local.sampled_submit_ns.push_back(
              std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                  .count());
        } else {
          rpc::common::LogInfo(payload);
        }
      }

      stats[static_cast<std::size_t>(t)] = std::move(local);
    });
  }

  while (ready.load(std::memory_order_acquire) < config.threads) {
    std::this_thread::yield();
  }

  const auto run_begin = Clock::now();
  go.store(true, std::memory_order_release);

  for (auto& worker : workers) {
    worker.join();
  }
  const auto run_end = Clock::now();

  const auto shutdown_begin = Clock::now();
  rpc::common::FlushLogger();
  rpc::common::ShutdownLogger();
  const auto shutdown_end = Clock::now();
  const rpc::common::LoggerRuntimeStats rt_stats =
      rpc::common::GetLoggerRuntimeStats();

  std::vector<std::int64_t> merged_samples;
  for (const auto& s : stats) {
    merged_samples.insert(merged_samples.end(), s.sampled_submit_ns.begin(),
                          s.sampled_submit_ns.end());
  }

  const std::int64_t total_messages =
      static_cast<std::int64_t>(config.threads) * config.messages_per_thread;
  const double run_seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(run_end - run_begin)
          .count();
  const double submit_throughput =
      run_seconds > 0.0 ? static_cast<double>(total_messages) / run_seconds : 0.0;
    const double enqueue_throughput =
      run_seconds > 0.0 ? static_cast<double>(rt_stats.enqueued) / run_seconds
              : 0.0;
    const double consume_throughput =
      run_seconds > 0.0 ? static_cast<double>(rt_stats.consumed) / run_seconds
              : 0.0;
    const double drop_rate =
      rt_stats.submit_calls > 0
        ? (100.0 * static_cast<double>(rt_stats.dropped) /
         static_cast<double>(rt_stats.submit_calls))
        : 0.0;

  double avg_submit_ns = 0.0;
  if (!merged_samples.empty()) {
    std::int64_t sum = 0;
    for (const auto ns : merged_samples) {
      sum += ns;
    }
    avg_submit_ns = static_cast<double>(sum) /
                    static_cast<double>(merged_samples.size());
  }

  std::error_code ec;
  const bool is_regular = std::filesystem::is_regular_file(config.log_file, ec);
  std::uintmax_t file_size = 0;
  bool file_ok = false;
  if (!ec && is_regular) {
    file_size = std::filesystem::file_size(config.log_file, ec);
    file_ok = !ec;
  }

  const auto run_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(run_end - run_begin)
          .count();
  const auto shutdown_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(shutdown_end - shutdown_begin)
          .count();

  std::cout << "=== async_logger_benchmark ===\n";
  std::cout << "threads=" << config.threads
            << " messages_per_thread=" << config.messages_per_thread
            << " message_bytes=" << config.message_bytes
            << " flush_ms=" << config.flush_ms << '\n';
  std::cout << "log_file=" << config.log_file << '\n';
  std::cout << "total_messages=" << total_messages << '\n';
  std::cout << "run_ms=" << run_ms << '\n';
  std::cout << "submit_throughput_msg_per_sec=" << submit_throughput << '\n';
  std::cout << "enqueue_throughput_msg_per_sec=" << enqueue_throughput << '\n';
  std::cout << "consume_throughput_msg_per_sec=" << consume_throughput << '\n';
  std::cout << "submit_calls=" << rt_stats.submit_calls << '\n';
  std::cout << "filtered_by_level=" << rt_stats.filtered_by_level << '\n';
  std::cout << "enqueued=" << rt_stats.enqueued << '\n';
  std::cout << "consumed=" << rt_stats.consumed << '\n';
  std::cout << "dropped=" << rt_stats.dropped << '\n';
  std::cout << "drop_rate_percent=" << drop_rate << '\n';
  std::cout << "sampled_avg_submit_ns=" << avg_submit_ns << '\n';
  std::cout << "sampled_p50_submit_ns=" << Percentile(merged_samples, 0.50)
            << '\n';
  std::cout << "sampled_p95_submit_ns=" << Percentile(merged_samples, 0.95)
            << '\n';
  std::cout << "sampled_p99_submit_ns=" << Percentile(merged_samples, 0.99)
            << '\n';
  std::cout << "shutdown_drain_ms=" << shutdown_ms << '\n';
  if (file_ok) {
    std::cout << "log_file_size_bytes=" << file_size << '\n';
  } else {
    std::cout << "log_file_size_bytes=NA\n";
  }

  std::cout << "csv=" << config.threads << ',' << config.messages_per_thread << ','
            << config.message_bytes << ',' << run_ms << ',' << submit_throughput
            << ',' << enqueue_throughput << ',' << consume_throughput << ','
            << rt_stats.submit_calls << ',' << rt_stats.enqueued << ','
            << rt_stats.consumed << ',' << rt_stats.dropped << ',' << drop_rate
            << ',' << avg_submit_ns << ',' << Percentile(merged_samples, 0.50)
            << ',' << Percentile(merged_samples, 0.95) << ','
            << Percentile(merged_samples, 0.99) << ',' << shutdown_ms << '\n';

  return 0;
}