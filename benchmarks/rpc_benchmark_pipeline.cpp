/**
 * @file rpc_benchmark_pipeline.cpp
 * @brief 单连接 + 多 in-flight 请求的吞吐 benchmark
 *
 * 定位：单连接多 in-flight benchmark
 * 测 CallAsync + PendingCalls + request_id 分发的价值
 * 重点观察 pipeline depth 提高后 QPS 是否提升，p95/p99 是否上升
 *
 * 模型：
 *   - 单个 RpcClient，单条连接
 *   - 使用 CallAsync() 连续发送请求
 *   - 控制 in-flight 窗口大小（1 / 8 / 32 / 64 / 128 / ...）
 *   - 发满窗口后回收最早的一批 future，再继续发送
 *   - 依赖现有 request_id / PendingCalls 机制正确匹配响应
 *
 * 运行：
 *   ./rpc_benchmark_pipeline                    # 自动扫描窗口序列
 *   ./rpc_benchmark_pipeline --requests=50000  # 指定请求数
 *   ./rpc_benchmark_pipeline --depth=32         # 只测单个窗口
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "client/rpc_client.h"
#include "common/log.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

// ============================================================================
// 配置与参数
// ============================================================================

struct BenchOptions {
  std::uint16_t port{50051};
  int total_requests{50000};
  int payload_bytes{64};
  /// 设为 0 表示自动扫一组窗口值；>0 表示只测单个窗口
  int fixed_depth{0};
  std::string output_dir{"benchmarks/results"};
};

struct DepthResult {
  int depth{0};              // pipeline depth
  int total_requests{0};
  int success_count{0};
  int failed_count{0};
  int timeout_count{0};
  double total_time_ms{0.0};
  double qps{0.0};
  double avg_latency_us{0.0};
  double p50_latency_us{0.0};
  double p95_latency_us{0.0};
  double p99_latency_us{0.0};
  long long max_latency_us{0};
};

bool CanConnect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const bool ok =
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool WaitServerReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

// ============================================================================
// 参数解析
// ============================================================================

bool ParseIntArg(const std::string& value, int* out) {
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || (end && *end != '\0')) return false;
  *out = static_cast<int>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, BenchOptions* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--port=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(7), &v)) return false;
      options->port = static_cast<std::uint16_t>(std::max(1, v));
    } else if (arg.rfind("--requests=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(11), &v)) return false;
      options->total_requests = std::max(1, v);
    } else if (arg.rfind("--depth=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(8), &v)) return false;
      options->fixed_depth = std::max(0, v);
    } else if (arg.rfind("--payload_bytes=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(16), &v)) return false;
      options->payload_bytes = std::max(1, v);
    } else if (arg.rfind("--output_dir=", 0) == 0) {
      options->output_dir = arg.substr(13);
    } else if (arg.rfind("--log=", 0) == 0) {
      // log level is handled globally via SetLogLevel, just accept the arg
      const std::string val = std::string(arg.substr(6));
      if (val == "off" || val == "error" || val == "info") {
        // Will be applied in main()
      } else {
        std::cerr << "unknown log level: " << val << "\n";
        return false;
      }
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  return true;
}

// ============================================================================
// 核心：pipeline benchmark 运行器
//
// 算法：
//   futures 环形缓冲区大小为 depth
//   1. 循环 total_requests 次：
//      a. 如果 i >= depth，回收 futures[i % depth]（get 结果）
//      b. 发起 CallAsync()，填入 futures[i % depth]
//   2. 循环结束后回收剩余的 future
//
// 这保证了任何时候连接上最多有 depth 个 in-flight 请求。
// ============================================================================

DepthResult RunPipelineBenchmark(rpc::client::RpcClient* client,
                                  std::string_view method_name,
                                  std::string_view payload, int depth,
                                  int total_requests) {
  DepthResult result;
  result.depth = depth;
  result.total_requests = total_requests;

  std::vector<std::future<rpc::client::RpcCallResult>> futures(
      static_cast<std::size_t>(depth));

  std::vector<long long> latencies_us;
  latencies_us.reserve(static_cast<std::size_t>(total_requests));

  // 记录每个 slot 的发送时间戳，用于计算真实 RTT
  std::vector<std::chrono::steady_clock::time_point> send_times(
      static_cast<std::size_t>(depth));

  const auto begin = std::chrono::steady_clock::now();

  for (int i = 0; i < total_requests; ++i) {
    const int slot = i % depth;

    // 回收旧 future（如果这个 slot 已经被占用过）
    if (i >= depth) {
      const auto recv_time = std::chrono::steady_clock::now();
      auto res = futures[static_cast<std::size_t>(slot)].get();

      const auto send_time = send_times[static_cast<std::size_t>(slot)];
      const auto latency =
          std::chrono::duration_cast<std::chrono::microseconds>(recv_time -
                                                                send_time)
              .count();
      latencies_us.push_back(latency);

      if (res.ok() && res.response_payload == payload) {
        ++result.success_count;
      } else if (res.status.message.find("timeout") != std::string::npos) {
        ++result.timeout_count;
      } else {
        ++result.failed_count;
      }
    }

    // 发起新请求
    send_times[static_cast<std::size_t>(slot)] =
        std::chrono::steady_clock::now();
    futures[static_cast<std::size_t>(slot)] =
        client->CallAsync("BenchmarkService", method_name, payload);
  }

  // 回收最后一批未完成的请求
  for (int i = total_requests - depth; i < total_requests; ++i) {
    if (i < 0) continue;
    const int slot = i % depth;
    const auto recv_time = std::chrono::steady_clock::now();
    auto res = futures[static_cast<std::size_t>(slot)].get();

    const auto send_time = send_times[static_cast<std::size_t>(slot)];
    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                             recv_time - send_time)
                             .count();
    latencies_us.push_back(latency);

    if (res.ok() && res.response_payload == payload) {
      ++result.success_count;
    } else if (res.status.message.find("timeout") != std::string::npos) {
      ++result.timeout_count;
    } else {
      ++result.failed_count;
    }
  }

  const auto end = std::chrono::steady_clock::now();
  result.total_time_ms =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count() /
      1000.0;
  result.qps = (result.total_time_ms > 0)
                   ? result.success_count * 1000.0 / result.total_time_ms
                   : 0.0;

  // 计算延迟统计
  if (!latencies_us.empty()) {
    std::sort(latencies_us.begin(), latencies_us.end());

    long long sum = 0;
    for (const auto us : latencies_us) sum += us;
    result.avg_latency_us =
        static_cast<double>(sum) / static_cast<double>(latencies_us.size());

    const auto pct = [&](double q) -> double {
      if (latencies_us.size() <= 1) return 0.0;
      const double idx = q * static_cast<double>(latencies_us.size() - 1);
      const std::size_t lo = static_cast<std::size_t>(idx);
      const std::size_t hi = std::min(lo + 1, latencies_us.size() - 1);
      const double frac = idx - static_cast<double>(lo);
      return static_cast<double>(latencies_us[lo]) * (1.0 - frac) +
             static_cast<double>(latencies_us[hi]) * frac;
    };

    result.p50_latency_us = pct(0.50);
    result.p95_latency_us = pct(0.95);
    result.p99_latency_us = pct(0.99);
    result.max_latency_us = latencies_us.back();
  }

  return result;
}

// ============================================================================
// 输出
// ============================================================================

void PrintHeader() {
  std::cout << std::left << std::setw(8) << "Depth" << std::right
            << std::setw(10) << "Reqs" << std::setw(10) << "OK"
            << std::setw(8) << "Fail" << std::setw(10) << "Timeout"
            << std::setw(12) << "Time(ms)" << std::setw(12) << "QPS"
            << std::setw(14) << "Avg(us)" << std::setw(14) << "P50(us)"
            << std::setw(14) << "P95(us)" << std::setw(14) << "P99(us)"
            << std::setw(14) << "Max(us)" << "\n";
  std::cout << std::string(140, '-') << "\n";
}

void PrintResult(const DepthResult& r) {
  std::cout << std::left << std::setw(8) << r.depth << std::right
            << std::setw(10) << r.total_requests << std::setw(10)
            << r.success_count << std::setw(8) << r.failed_count << std::setw(10)
            << r.timeout_count << std::fixed << std::setprecision(1)
            << std::setw(12) << r.total_time_ms << std::setw(12)
            << std::setprecision(0) << r.qps << std::setprecision(1)
            << std::setw(14) << r.avg_latency_us << std::setw(14)
            << r.p50_latency_us << std::setw(14) << r.p95_latency_us
            << std::setw(14) << r.p99_latency_us << std::setw(14)
            << r.max_latency_us << "\n";
}

void PrintCsvHeader() {
  std::cout << "depth,total_requests,success,failed,timeout,time_ms,qps,"
            << "avg_us,p50_us,p95_us,p99_us,max_us\n";
}

void PrintCsv(const DepthResult& r) {
  std::cout << r.depth << "," << r.total_requests << "," << r.success_count
            << "," << r.failed_count << "," << r.timeout_count << ","
            << std::fixed << std::setprecision(1) << r.total_time_ms << ","
            << std::setprecision(0) << r.qps << "," << std::setprecision(1)
            << r.avg_latency_us << "," << r.p50_latency_us << ","
            << r.p95_latency_us << "," << r.p99_latency_us << ","
            << r.max_latency_us << "\n";
}

void PrintUsage() {
  std::cout << "usage: rpc_benchmark_pipeline "
               "[--port=N] [--requests=N] [--depth=N] [--payload_bytes=N] "
               "[--output_dir=PATH]\n"
            << "  --depth=0  自动扫描窗口序列 (默认: 1, 8, 32, 64, 128, 256)\n"
            << "  --depth=N  只测指定窗口大小\n";
}

// ============================================================================
// 结果文件输出
// ============================================================================

bool EnsureDirExists(const std::string& path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(path), ec);
  return !ec;
}

std::string BuildFileStem(const std::vector<int>& depths, int total_requests,
                           int payload_bytes) {
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::string stem = "rpc_benchmark_pipeline_r" +
                     std::to_string(total_requests) + "_p" +
                     std::to_string(payload_bytes) + "_d";
  for (size_t i = 0; i < depths.size(); ++i) {
    if (i > 0) stem += "-";
    stem += std::to_string(depths[i]);
  }
  stem += "_" + std::to_string(now_ms);
  return stem;
}

bool WriteResultFiles(const std::vector<DepthResult>& results,
                      const std::string& output_dir,
                      const std::string& file_stem) {
  if (!EnsureDirExists(output_dir)) {
    std::cerr << "failed to create output dir: " << output_dir << "\n";
    return false;
  }

  const std::string text_path = output_dir + "/" + file_stem + ".txt";
  const std::string csv_path = output_dir + "/" + file_stem + ".csv";

  // --- text ---
  std::ofstream tf(text_path);
  if (!tf) {
    std::cerr << "failed to open " << text_path << "\n";
    return false;
  }
  std::ostringstream oss;
  oss << std::left << std::setw(8) << "Depth" << std::right << std::setw(10)
      << "Reqs" << std::setw(10) << "OK" << std::setw(8) << "Fail"
      << std::setw(10) << "Timeout" << std::setw(12) << "Time(ms)"
      << std::setw(12) << "QPS" << std::setw(14) << "Avg(us)"
      << std::setw(14) << "P50(us)" << std::setw(14) << "P95(us)"
      << std::setw(14) << "P99(us)" << std::setw(14) << "Max(us)" << "\n";
  oss << std::string(140, '-') << "\n";
  for (const auto& r : results) {
    oss << std::left << std::setw(8) << r.depth << std::right << std::setw(10)
        << r.total_requests << std::setw(10) << r.success_count << std::setw(8)
        << r.failed_count << std::setw(10) << r.timeout_count << std::fixed
        << std::setprecision(1) << std::setw(12) << r.total_time_ms
        << std::setw(12) << std::setprecision(0) << r.qps << std::setprecision(1)
        << std::setw(14) << r.avg_latency_us << std::setw(14)
        << r.p50_latency_us << std::setw(14) << r.p95_latency_us
        << std::setw(14) << r.p99_latency_us << std::setw(14)
        << r.max_latency_us << "\n";
  }
  tf << oss.str();
  tf.close();

  // --- csv ---
  std::ofstream cf(csv_path);
  if (!cf) {
    std::cerr << "failed to open " << csv_path << "\n";
    return false;
  }
  cf << "depth,total_requests,success,failed,timeout,time_ms,qps,"
        "avg_us,p50_us,p95_us,p99_us,max_us\n";
  for (const auto& r : results) {
    cf << r.depth << "," << r.total_requests << "," << r.success_count << ","
       << r.failed_count << "," << r.timeout_count << "," << std::fixed
       << std::setprecision(1) << r.total_time_ms << "," << std::setprecision(0)
       << r.qps << "," << std::setprecision(1) << r.avg_latency_us << ","
       << r.p50_latency_us << "," << r.p95_latency_us << ","
       << r.p99_latency_us << "," << r.max_latency_us << "\n";
  }
  cf.close();

  std::cout << "result_text_file=" << text_path << "\n";
  std::cout << "result_csv_file=" << csv_path << "\n";
  return true;
}

// ============================================================================
// 入口
// ============================================================================

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

  BenchOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage();
    return 1;
  }

  // ---------- 内嵌 benchmark server ----------
  rpc::server::ServiceRegistry registry;
  if (!registry.Register(
          "BenchmarkService", "Echo",
          [](std::string_view payload) { return std::string(payload); })) {
    std::cerr << "failed to register benchmark method\n";
    return 1;
  }

  constexpr std::size_t kWorkerCount = 2;
  rpc::server::RpcServer server(options.port, registry, kWorkerCount);

  bool server_ok = false;
  std::thread server_thread([&]() { server_ok = server.Start(); });

  if (!WaitServerReady(options.port, std::chrono::seconds(3))) {
    std::cerr << "server not ready on port " << options.port << "\n";
    return 1;
  }

  // ---------- 构建 payload ----------
  std::string payload =
      std::string(static_cast<std::size_t>(options.payload_bytes), 'x');

  // ---------- 创建客户端 ----------
  rpc::client::RpcClient client(
      "127.0.0.1", options.port,
      {.send_timeout = std::chrono::milliseconds(5000),
       .recv_timeout = std::chrono::milliseconds(5000),
       .heartbeat_interval = std::chrono::seconds(0),
       .heartbeat_timeout = std::chrono::seconds(0)});

  // ---------- 选择窗口序列 ----------
  std::vector<int> depths;
  if (options.fixed_depth > 0) {
    depths.push_back(options.fixed_depth);
  } else {
    depths = {1, 8, 32, 64, 128, 256};
  }

  // ---------- 预热 ----------
  for (int i = 0; i < 10; ++i) {
    [[maybe_unused]] auto _ = client.Call("BenchmarkService", "Echo", payload);
  }

  // ---------- 运行 ----------
  std::cout << "\n=== Pipeline Benchmark: "
            << "requests=" << options.total_requests
            << ", payload=" << payload.size() << " bytes ===\n\n";

  PrintHeader();

  std::vector<DepthResult> all_results;
  all_results.reserve(depths.size());

  for (const int d : depths) {
    if (d > options.total_requests) {
      continue;
    }
    auto r = RunPipelineBenchmark(&client, "Echo", payload, d,
                                  options.total_requests);

    // Check for failures
    if (r.failed_count > 0 || r.timeout_count > 0) {
      std::cerr << "WARNING: depth=" << d << " has " << r.failed_count
                << " failed, " << r.timeout_count << " timeouts\n";
    }

    all_results.push_back(std::move(r));
    PrintResult(all_results.back());
  }

  std::cout << "\n=== CSV ===\n";
  PrintCsvHeader();
  for (const auto& r : all_results) {
    PrintCsv(r);
  }
  std::cout << "\n";

  // ---------- 保存结果到文件 ----------
  const std::string file_stem =
      BuildFileStem(depths, options.total_requests, options.payload_bytes);
  WriteResultFiles(all_results, options.output_dir, file_stem);

  // ---------- 清理 ----------
  server.Stop();
  server_thread.join();

  std::cout << "done.\n";
  return 0;
}