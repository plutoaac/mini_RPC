/**
 * @file rpc_benchmark.cpp
 * @brief 基础 RPC 延迟与吞吐 benchmark
 *
 * 定位：baseline benchmark，测最基础的 RPC 调用延迟和吞吐
 *
 * 模型：
 *   - 启动内嵌服务端
 *   - 创建 N 个并发 client 线程
 *   - 每个线程使用 sync/async/coroutine 模式调用
 *   - 测量端到端延迟和 QPS
 *
 * 运行：
 *   ./rpc_benchmark 1000                          # 默认 sync 模式，1000 请求
 *   ./rpc_benchmark --mode=sync --requests=5000    # sync 模式
 *   ./rpc_benchmark --mode=async --requests=5000   # async 模式
 *   ./rpc_benchmark --mode=co --requests=5000      # coroutine 模式
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "benchmark_stats.h"
#include "client/rpc_client.h"
#include "common/log.h"
#include "coroutine/task.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

enum class BenchMode {
  kSync,
  kAsync,
  kCoroutine,
};

struct BenchmarkOptions {
  BenchMode mode{BenchMode::kSync};
  int total_requests{10000};
  int concurrency{8};
  int payload_bytes{128};
  std::uint16_t port{50051};
  std::string output_dir{"benchmarks/results"};
};

bool CanConnect(const std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  const bool ok =
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool WaitServerReady(const std::uint16_t port,
                     const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string BuildPayload(const int payload_bytes) {
  const int bytes = std::max(1, payload_bytes);
  return std::string(static_cast<std::size_t>(bytes), 'x');
}

bool IsTimeoutResult(const rpc::client::RpcCallResult& result) {
  return result.status.message.find("timeout") != std::string::npos;
}

std::string ModeToString(const BenchMode mode) {
  switch (mode) {
    case BenchMode::kSync:
      return "sync";
    case BenchMode::kAsync:
      return "async";
    case BenchMode::kCoroutine:
      return "co";
  }
  return "unknown";
}

bool ParseMode(const std::string& text, BenchMode* mode) {
  if (text == "sync") {
    *mode = BenchMode::kSync;
    return true;
  }
  if (text == "async") {
    *mode = BenchMode::kAsync;
    return true;
  }
  if (text == "co") {
    *mode = BenchMode::kCoroutine;
    return true;
  }
  return false;
}

bool ParseIntArg(const std::string& value, int* out) {
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

bool ParseArgs(const int argc, char** argv, BenchmarkOptions* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--mode=", 0) == 0) {
      if (!ParseMode(arg.substr(7), &options->mode)) {
        std::cerr << "invalid mode: " << arg.substr(7) << "\n";
        return false;
      }
      continue;
    }
    if (arg.rfind("--requests=", 0) == 0) {
      int value = 0;
      if (!ParseIntArg(arg.substr(11), &value)) {
        return false;
      }
      options->total_requests = std::max(1, value);
      continue;
    }
    if (arg.rfind("--concurrency=", 0) == 0) {
      int value = 0;
      if (!ParseIntArg(arg.substr(14), &value)) {
        return false;
      }
      options->concurrency = std::max(1, value);
      continue;
    }
    if (arg.rfind("--payload_bytes=", 0) == 0) {
      int value = 0;
      if (!ParseIntArg(arg.substr(16), &value)) {
        return false;
      }
      options->payload_bytes = std::max(1, value);
      continue;
    }
    if (arg.rfind("--port=", 0) == 0) {
      int value = 0;
      if (!ParseIntArg(arg.substr(7), &value)) {
        return false;
      }
      options->port = static_cast<std::uint16_t>(std::max(1, value));
      continue;
    }
    if (arg.rfind("--output_dir=", 0) == 0) {
      options->output_dir = arg.substr(13);
      continue;
    }
    if (arg.rfind("--log=", 0) == 0) {
      // log level handled in main, just accept the argument
      continue;
    }

    int value = 0;
    if (!ParseIntArg(arg, &value)) {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
    options->total_requests = std::max(1, value);
  }
  return true;
}

rpc::benchmark::LocalStats RunSyncWorker(rpc::client::RpcClient* client,
                                         const std::string& payload,
                                         const int request_count) {
  rpc::benchmark::LocalStats stats;
  stats.latency_us.reserve(static_cast<std::size_t>(request_count));

  for (int i = 0; i < request_count; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto result = client->Call("BenchmarkService", "Echo", payload);
    const auto t2 = std::chrono::steady_clock::now();

    stats.latency_us.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());

    if (result.ok() && result.response_payload == payload) {
      ++stats.success_count;
    } else if (IsTimeoutResult(result)) {
      ++stats.timeout_count;
    } else {
      ++stats.failed_count;
    }
  }

  return stats;
}

rpc::benchmark::LocalStats RunAsyncWorker(rpc::client::RpcClient* client,
                                           const std::string& payload,
                                           const int request_count) {
  rpc::benchmark::LocalStats stats;
  stats.latency_us.reserve(static_cast<std::size_t>(request_count));

  for (int i = 0; i < request_count; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    auto future = client->CallAsync("BenchmarkService", "Echo", payload);
    const auto result = future.get();
    const auto t2 = std::chrono::steady_clock::now();

    stats.latency_us.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());

    if (result.ok() && result.response_payload == payload) {
      ++stats.success_count;
    } else if (IsTimeoutResult(result)) {
      ++stats.timeout_count;
    } else {
      ++stats.failed_count;
    }
  }

  return stats;
}

rpc::coroutine::Task<rpc::benchmark::LocalStats> RunCoroutineWorker(
    rpc::client::RpcClient* client, const std::string* payload,
    const int request_count) {
  rpc::benchmark::LocalStats stats;
  stats.latency_us.reserve(static_cast<std::size_t>(request_count));

  for (int i = 0; i < request_count; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto result =
        co_await client->CallCo("BenchmarkService", "Echo", *payload);
    const auto t2 = std::chrono::steady_clock::now();

    stats.latency_us.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());

    if (result.ok() && result.response_payload == *payload) {
      ++stats.success_count;
    } else if (IsTimeoutResult(result)) {
      ++stats.timeout_count;
    } else {
      ++stats.failed_count;
    }
  }

  co_return stats;
}

rpc::benchmark::BenchmarkResult RunBenchmark(const BenchmarkOptions& options) {
  rpc::server::ServiceRegistry registry;
  if (!registry.Register("BenchmarkService", "Echo",
                         [](const std::string_view payload) {
                           return std::string(payload);
                         })) {
    std::cerr << "failed to register benchmark method\n";
    std::abort();
  }

  const std::size_t worker_count =
      static_cast<std::size_t>(std::max(2, std::min(options.concurrency, 8)));
  rpc::server::RpcServer server(options.port, registry, worker_count);

  bool server_start_ok = false;
  std::thread server_thread([&]() { server_start_ok = server.Start(); });

  if (!WaitServerReady(options.port, std::chrono::seconds(2))) {
    std::cerr << "benchmark server not ready\n";
    std::abort();
  }

  // Warmup: send a few requests before timing
  {
    rpc::client::RpcClient warmup_client(
        "127.0.0.1", options.port,
        {.send_timeout = std::chrono::milliseconds(3000),
         .recv_timeout = std::chrono::milliseconds(3000)});
    const std::string warmup_payload = BuildPayload(options.payload_bytes);
    for (int i = 0; i < 5; ++i) {
      [[maybe_unused]] auto _ = warmup_client.Call("BenchmarkService", "Echo",
                                                  warmup_payload);
    }
  }

  // connections=1 (single connection benchmark), depth=1
  rpc::benchmark::BenchmarkStats stats(
      ModeToString(options.mode), 1, 1, options.concurrency,
      options.payload_bytes, options.total_requests);

  const std::string payload = BuildPayload(options.payload_bytes);
  const int base = options.total_requests / options.concurrency;
  const int rem = options.total_requests % options.concurrency;

  std::vector<rpc::benchmark::LocalStats> locals(
      static_cast<std::size_t>(options.concurrency));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(options.concurrency));

  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < options.concurrency; ++i) {
    const int request_count = base + ((i < rem) ? 1 : 0);
    threads.emplace_back([&, i, request_count]() {
      rpc::client::RpcClient client(
          "127.0.0.1", options.port,
          {.send_timeout = std::chrono::milliseconds(3000),
           .recv_timeout = std::chrono::milliseconds(3000)});

      if (options.mode == BenchMode::kSync) {
        locals[static_cast<std::size_t>(i)] =
            RunSyncWorker(&client, payload, request_count);
        return;
      }
      if (options.mode == BenchMode::kAsync) {
        locals[static_cast<std::size_t>(i)] =
            RunAsyncWorker(&client, payload, request_count);
        return;
      }

      locals[static_cast<std::size_t>(i)] = rpc::coroutine::SyncWait(
          RunCoroutineWorker(&client, &payload, request_count));
    });
  }

  for (auto& t : threads) {
    t.join();
  }
  const auto end = std::chrono::steady_clock::now();

  for (auto& local : locals) {
    stats.Merge(std::move(local));
  }

  if (!server.Stop()) {
    std::cerr << "failed to stop benchmark server\n";
  }
  server_thread.join();
  if (!server_start_ok) {
    std::cerr << "benchmark server start exited unexpectedly\n";
  }

  return stats.Finalize(begin, end);
}

void PrintUsage() {
  std::cout << "usage: rpc_benchmark [--mode=sync|async|co] [--requests=N] "
               "[--concurrency=N] [--payload_bytes=N] [--port=N] "
               "[--output_dir=PATH]\n"
            << "  --mode=sync|async|co  Call mode (default: sync)\n"
            << "  --requests=N          Total requests (default: 10000)\n"
            << "  --concurrency=N       Number of client threads (default: 8)\n"
            << "  --payload_bytes=N     Payload size in bytes (default: 128)\n"
            << "  --port=N              Server port (default: 50051)\n"
            << "  --output_dir=PATH     Output directory (default: benchmarks/results)\n";
}

}  // namespace

static rpc::common::LogLevel ParseLogArg(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg.rfind("--log=", 0) == 0) {
      std::string val = arg.substr(6);
      if (val == "off") return rpc::common::LogLevel::kOff;
      if (val == "error") return rpc::common::LogLevel::kError;
      if (val == "info") return rpc::common::LogLevel::kInfo;
    }
  }
  return rpc::common::LogLevel::kOff;  // Default to off for clean benchmarks
}

int main(int argc, char** argv) {
  // Apply log level from --log argument (default: off)
  rpc::common::SetLogLevel(ParseLogArg(argc, argv));

  BenchmarkOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage();
    return 1;
  }

  const auto result = RunBenchmark(options);
  rpc::benchmark::PrintBenchmarkResult(result);
  rpc::benchmark::PrintBenchmarkCsv(result);

  // Check for failures
  if (result.failed_count > 0 || result.timeout_count > 0) {
    std::cerr << "WARNING: " << result.failed_count << " failed, "
              << result.timeout_count << " timeouts\n";
  }

  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string file_stem = "rpc_benchmark_" + result.mode + "_c" +
                                std::to_string(result.concurrency) + "_p" +
                                std::to_string(result.payload_bytes) + "_r" +
                                std::to_string(result.total_requests) + "_" +
                                std::to_string(now_ms);
  std::string text_path;
  std::string csv_path;
  if (rpc::benchmark::WriteBenchmarkResultFiles(
          result, options.output_dir, file_stem, &text_path, &csv_path)) {
    std::cout << "result_text_file=" << text_path << '\n';
    std::cout << "result_csv_file=" << csv_path << '\n';
  } else {
    std::cerr << "failed to write benchmark result files to "
              << options.output_dir << '\n';
  }
  return 0;
}