/**
 * @file rpc_benchmark_conn_pool.cpp
 * @brief 多连接 × 每连接固定 in-flight 的吞吐上限 benchmark
 *
 * 目标：判断瓶颈是否在"单连接发送/接收路径"上。
 *
 * 模型：
 *   - 创建 N 个 RpcClient，每个独占一条 TCP 连接
 *   - 每个连接使用 CallAsync() 环形 pipeline 发请求
 *   - 每连接 in-flight 深度固定
 *   - 总并发 = 连接数 × 每连接深度
 *
 * 与已有 benchmark 的对照：
 *   - rpc_benchmark            ：多 client 串行同步/异步调用
 *   - rpc_benchmark_pipeline   ：单 client 多 in-flight
 *   - rpc_benchmark_conn_pool  ：多 client × 每连接多 in-flight
 *
 * 运行：
 *   ./rpc_benchmark_conn_pool                                  # 扫矩阵
 *   ./rpc_benchmark_conn_pool --conns=8 --depth=32 --requests=100000
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "benchmark_stats.h"
#include "client/rpc_client.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

// ============================================================================
// 配置
// ============================================================================

struct BenchOptions {
  std::uint16_t port{50051};
  int total_requests{50000};
  int payload_bytes{64};
  /// 设为 0 表示扫矩阵；>0 表示固定值
  int fixed_conns{0};
  int fixed_depth{0};
  std::string output_dir;
};

struct BenchPoint {
  int connections{0};
  int depth{0};  // per-connection in-flight
};

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
    } else if (arg.rfind("--payload_bytes=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(16), &v)) return false;
      options->payload_bytes = std::max(1, v);
    } else if (arg.rfind("--conns=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(8), &v)) return false;
      options->fixed_conns = std::max(0, v);
    } else if (arg.rfind("--depth=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(8), &v)) return false;
      options->fixed_depth = std::max(0, v);
    } else if (arg.rfind("--output_dir=", 0) == 0) {
      options->output_dir = arg.substr(13);
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  return true;
}

// ============================================================================
// 辅助函数（与 rpc_benchmark.cpp 共用逻辑）
// ============================================================================

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
// 单连接 pipeline worker
//
// 环形缓冲区：大小为 depth。循环 total_per_conn 次，每次先回收旧 future
// 再发新。 最后回收尾部未完成的 future。 返回值是
// LocalStats，延迟从发送到收到之间的 RTT。
// ============================================================================

rpc::benchmark::LocalStats RunConnPipelineWorker(rpc::client::RpcClient* client,
                                                 std::string_view service_name,
                                                 std::string_view method_name,
                                                 std::string_view payload,
                                                 int depth,
                                                 int total_per_conn) {
  rpc::benchmark::LocalStats stats;
  stats.latency_us.reserve(static_cast<std::size_t>(total_per_conn));

  std::vector<std::future<rpc::client::RpcCallResult>> futures(
      static_cast<std::size_t>(depth));
  std::vector<std::chrono::steady_clock::time_point> send_times(
      static_cast<std::size_t>(depth));

  for (int i = 0; i < total_per_conn; ++i) {
    const int slot = i % depth;

    if (i >= depth) {
      // 回收旧请求
      const auto recv_time = std::chrono::steady_clock::now();
      auto res = futures[static_cast<std::size_t>(slot)].get();
      const auto send_time = send_times[static_cast<std::size_t>(slot)];
      const auto latency =
          std::chrono::duration_cast<std::chrono::microseconds>(recv_time -
                                                                send_time)
              .count();
      stats.latency_us.push_back(latency);

      if (res.ok() && res.response_payload == payload) {
        ++stats.success_count;
      } else {
        ++stats.failed_count;
      }
    }

    send_times[static_cast<std::size_t>(slot)] =
        std::chrono::steady_clock::now();
    futures[static_cast<std::size_t>(slot)] =
        client->CallAsync(service_name, method_name, payload);
  }

  // 回收尾部
  for (int i = total_per_conn - depth; i < total_per_conn; ++i) {
    if (i < 0) continue;
    const int slot = i % depth;
    const auto recv_time = std::chrono::steady_clock::now();
    auto res = futures[static_cast<std::size_t>(slot)].get();
    const auto send_time = send_times[static_cast<std::size_t>(slot)];
    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                             recv_time - send_time)
                             .count();
    stats.latency_us.push_back(latency);

    if (res.ok() && res.response_payload == payload) {
      ++stats.success_count;
    } else {
      ++stats.failed_count;
    }
  }

  return stats;
}

// ============================================================================
// 单测点运行器
// ============================================================================

rpc::benchmark::BenchmarkResult RunPoint(std::uint16_t port,
                                         std::string_view service_name,
                                         std::string_view method_name,
                                         const std::string& payload,
                                         int connections, int depth,
                                         int total_requests) {
  // total_requests 按连接数平均分配
  const int base = total_requests / connections;
  const int rem = total_requests % connections;

  // 每个连接创建独立 RpcClient（用 unique_ptr，因为 RpcClient
  // 不可拷贝不可移动）
  std::vector<std::unique_ptr<rpc::client::RpcClient>> clients;
  clients.reserve(static_cast<std::size_t>(connections));
  for (int i = 0; i < connections; ++i) {
    clients.push_back(std::make_unique<rpc::client::RpcClient>(
        "127.0.0.1", port,
        rpc::client::RpcClientOptions{
            .send_timeout = std::chrono::milliseconds(5000),
            .recv_timeout = std::chrono::milliseconds(5000)}));
  }

  // 预热（每个连接发 3 个请求）
  for (const auto& c : clients) {
    for (int i = 0; i < 3; ++i) {
      [[maybe_unused]] auto _ =
          c->Call(std::string(service_name), std::string(method_name), payload);
    }
  }

  // 多线程并发运行每个连接的 pipeline
  std::vector<rpc::benchmark::LocalStats> locals(
      static_cast<std::size_t>(connections));
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(connections));

  const auto begin = std::chrono::steady_clock::now();

  for (int i = 0; i < connections; ++i) {
    const int req_count = base + (i < rem ? 1 : 0);
    threads.emplace_back([&clients, &locals, &service_name, &method_name,
                          &payload, depth, req_count, i]() {
      locals[static_cast<std::size_t>(i)] = RunConnPipelineWorker(
          clients[static_cast<std::size_t>(i)].get(), service_name, method_name,
          payload, depth, req_count);
    });
  }

  for (auto& t : threads) {
    t.join();
  }
  const auto end = std::chrono::steady_clock::now();

  // 聚合
  rpc::benchmark::BenchmarkStats agg("conn_pool", connections,
                                     static_cast<int>(payload.size()),
                                     total_requests);
  for (auto& local : locals) {
    agg.Merge(std::move(local));
  }

  return agg.Finalize(begin, end);
}

// ============================================================================
// 输出
// ============================================================================

void PrintTableHeader() {
  std::cout << std::left << std::setw(10) << "Conns" << std::right
            << std::setw(10) << "Depth" << std::setw(10) << "Total"
            << std::setw(10) << "OK" << std::setw(8) << "Fail" << std::setw(12)
            << "Time(ms)" << std::setw(12) << "QPS" << std::setw(14)
            << "Avg(us)" << std::setw(14) << "P50(us)" << std::setw(14)
            << "P95(us)" << std::setw(14) << "P99(us)" << std::setw(14)
            << "Max(us)"
            << "\n";
  std::cout << std::string(140, '-') << "\n";
}

void PrintUsage() {
  std::cout << "usage: rpc_benchmark_conn_pool "
               "[--port=N] [--requests=N] [--conns=N] [--depth=N] "
               "[--payload_bytes=N] [--output_dir=PATH]\n"
            << "  --conns=0 / --depth=0  自动扫矩阵 (默认)\n"
            << "  --conns=N --depth=N    固定单测点\n";
}

}  // namespace

int main(int argc, char** argv) {
  BenchOptions options;
  // 自动检测输出目录
  if (access("benchmarks/results", F_OK) == 0) {
    options.output_dir = "benchmarks/results";
  } else if (access("../benchmarks/results", F_OK) == 0) {
    options.output_dir = "../benchmarks/results";
  } else {
    options.output_dir = ".";
  }

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

  constexpr std::size_t kWorkerCount = 4;
  rpc::server::RpcServer server(options.port, registry, kWorkerCount);

  bool server_ok = false;
  std::thread server_thread([&]() { server_ok = server.Start(); });

  if (!WaitServerReady(options.port, std::chrono::seconds(3))) {
    std::cerr << "server not ready on port " << options.port << "\n";
    return 1;
  }

  // ---------- payload ----------
  std::string payload =
      std::string(static_cast<std::size_t>(options.payload_bytes), 'x');

  // ---------- 测点矩阵 ----------
  std::vector<BenchPoint> points;

  if (options.fixed_conns > 0 && options.fixed_depth > 0) {
    points.push_back({options.fixed_conns, options.fixed_depth});
  } else {
    // 默认矩阵：连接数 × 深度
    const std::vector<int> conn_values =
        options.fixed_conns > 0 ? std::vector<int>{options.fixed_conns}
                                : std::vector<int>{1, 4, 8, 16};
    const std::vector<int> depth_values =
        options.fixed_depth > 0 ? std::vector<int>{options.fixed_depth}
                                : std::vector<int>{8, 16, 32};

    for (int c : conn_values) {
      for (int d : depth_values) {
        points.push_back({c, d});
      }
    }
  }

  // ---------- 运行 ----------
  std::cout << "\n=== Connection Pool Benchmark: "
            << "requests=" << options.total_requests
            << ", payload=" << payload.size() << " bytes ===\n\n";

  PrintTableHeader();

  std::vector<rpc::benchmark::BenchmarkResult> all_results;
  all_results.reserve(points.size());

  for (const auto& p : points) {
    auto r = RunPoint(options.port, "BenchmarkService", "Echo", payload,
                      p.connections, p.depth, options.total_requests);
    all_results.push_back(std::move(r));
  }

  // 格式化输出表格
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& r = all_results[i];
    const auto& p = points[i];

    std::cout << std::left << std::setw(10) << p.connections << std::right
              << std::setw(10) << p.depth << std::setw(10) << r.total_requests
              << std::setw(10) << r.success_count << std::setw(8)
              << r.failed_count << std::fixed << std::setprecision(1)
              << std::setw(12) << r.total_time_ms << std::setw(12)
              << std::setprecision(0) << r.qps << std::setprecision(1)
              << std::setw(14) << r.avg_latency_us << std::setw(14)
              << r.p50_latency_us << std::setw(14) << r.p95_latency_us
              << std::setw(14) << r.p99_latency_us << std::setw(14)
              << r.max_latency_us << "\n";
  }

  std::cout << "\n=== CSV ===\n";
  std::cout << "conns,depth,total,ok,fail,time_ms,qps,avg_us,p50_us,p95_us,"
               "p99_us,max_us\n";
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& r = all_results[i];
    const auto& p = points[i];
    std::cout << p.connections << "," << p.depth << "," << r.total_requests
              << "," << r.success_count << "," << r.failed_count << ","
              << std::fixed << std::setprecision(1) << r.total_time_ms << ","
              << std::setprecision(0) << r.qps << "," << std::setprecision(1)
              << r.avg_latency_us << "," << r.p50_latency_us << ","
              << r.p95_latency_us << "," << r.p99_latency_us << ","
              << r.max_latency_us << "\n";
  }
  std::cout << "\n";

  // ---------- 保存结果到文件 ----------
  // 把每个测点写成一个独立文件（复用已有格式）
  for (size_t i = 0; i < points.size(); ++i) {
    const auto& r = all_results[i];
    const auto& p = points[i];
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    const std::string stem =
        "rpc_benchmark_conn_pool_c" + std::to_string(p.connections) + "_d" +
        std::to_string(p.depth) + "_r" + std::to_string(r.total_requests) +
        "_p" + std::to_string(r.payload_bytes) + "_" + std::to_string(now_ms);

    std::string text_path;
    std::string csv_path;
    rpc::benchmark::WriteBenchmarkResultFiles(r, options.output_dir, stem,
                                              &text_path, &csv_path);
    std::cout << "result[" << i << "] text=" << text_path << " csv=" << csv_path
              << "\n";
  }

  // ---------- 清理 ----------
  server.Stop();
  server_thread.join();

  std::cout << "\ndone.\n";
  return 0;
}
