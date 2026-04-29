/**
 * @file rpc_client_pool_benchmark.cpp
 * @brief RpcClientPool 负载均衡 benchmark
 *
 * 定位：展示 RpcClientPool 负载均衡模块的作用
 * - 不追求极限 QPS，而是展示策略行为
 * - 重点观察分发比例、健康状态、故障转移
 *
 * RpcClientPool 说明：
 *   RpcClientPool 持有多个 RpcClient（每个 endpoint 独占一个）
 *   支持 RoundRobin / LeastInflight 负载均衡策略
 *   支持最小健康检查和故障转移
 *
 * 场景：
 *   1. round_robin: 展示 RoundRobin 分发到多个 endpoint
 *   2. least_inflight: 展示 LeastInflight 倾向选择轻载节点
 *   3. failover: 展示故障转移和健康检查
 *
 * 运行：
 *   ./rpc_client_pool_benchmark --scenario=round_robin --requests=30000 --payload_bytes=64
 *   ./rpc_client_pool_benchmark --scenario=least_inflight --requests=30000 --payload_bytes=64
 *   ./rpc_client_pool_benchmark --scenario=failover --requests=1000 --payload_bytes=64
 */

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "benchmark_stats.h"
#include "client/rpc_client.h"
#include "client/rpc_client_pool.h"
#include "common/log.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

// ============================================================================
// 配置
// ============================================================================

enum class BenchmarkScenario {
  kRoundRobin,
  kLeastInflight,
  kFailover,
};

struct BenchOptions {
  BenchmarkScenario scenario{BenchmarkScenario::kRoundRobin};
  int total_requests{30000};
  int payload_bytes{64};
  int concurrency{4};  // 匹配 4 核 CPU
  std::string output_dir{"benchmarks/results"};
};

struct PoolBenchmarkResult {
  std::string scenario;
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
  int payload_bytes{64};
  std::vector<rpc::client::RpcClientPool::EndpointStats> endpoint_stats;
};

// ============================================================================
// 辅助函数
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
// 参数解析
// ============================================================================

bool ParseScenario(const std::string& text, BenchmarkScenario* scenario) {
  if (text == "round_robin") {
    *scenario = BenchmarkScenario::kRoundRobin;
    return true;
  }
  if (text == "least_inflight") {
    *scenario = BenchmarkScenario::kLeastInflight;
    return true;
  }
  if (text == "failover") {
    *scenario = BenchmarkScenario::kFailover;
    return true;
  }
  return false;
}

bool ParseIntArg(const std::string& value, int* out) {
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || (end && *end != '\0')) return false;
  *out = static_cast<int>(parsed);
  return true;
}

// 从延迟数据计算统计指标（P50/P95/P99/avg/max）
void ComputeLatencyStats(std::vector<long long>& latencies,
                         PoolBenchmarkResult* result) {
  if (latencies.empty()) return;

  std::sort(latencies.begin(), latencies.end());

  long long sum = 0;
  for (const auto us : latencies) sum += us;
  result->avg_latency_us =
      static_cast<double>(sum) / static_cast<double>(latencies.size());

  const auto percentile = [&](double q) -> double {
    if (latencies.size() <= 1) return 0.0;
    const double idx = q * static_cast<double>(latencies.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(idx);
    const std::size_t hi = std::min(lo + 1, latencies.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return static_cast<double>(latencies[lo]) * (1.0 - frac) +
           static_cast<double>(latencies[hi]) * frac;
  };

  result->p50_latency_us = percentile(0.50);
  result->p95_latency_us = percentile(0.95);
  result->p99_latency_us = percentile(0.99);
  result->max_latency_us = latencies.back();
}

// 构建 PoolBenchmarkResult 公共字段
PoolBenchmarkResult BuildResult(const std::string& scenario,
                                const BenchOptions& options,
                                int success_count, int failed_count,
                                int timeout_count,
                                std::chrono::steady_clock::time_point begin,
                                std::chrono::steady_clock::time_point end) {
  PoolBenchmarkResult result;
  result.scenario = scenario;
  result.total_requests = options.total_requests;
  result.success_count = success_count;
  result.failed_count = failed_count;
  result.timeout_count = timeout_count;
  result.payload_bytes = options.payload_bytes;
  result.total_time_ms =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count() / 1000.0;
  result.qps = result.total_time_ms > 0
                   ? result.success_count * 1000.0 / result.total_time_ms
                   : 0.0;
  return result;
}

bool ParseArgs(int argc, char** argv, BenchOptions* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind("--scenario=", 0) == 0) {
      if (!ParseScenario(arg.substr(11), &options->scenario)) {
        std::cerr << "invalid scenario: " << arg.substr(11) << "\n";
        return false;
      }
    } else if (arg.rfind("--requests=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(11), &v)) return false;
      options->total_requests = std::max(1, v);
    } else if (arg.rfind("--payload_bytes=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(16), &v)) return false;
      options->payload_bytes = std::max(1, v);
    } else if (arg.rfind("--concurrency=", 0) == 0) {
      int v = 0;
      if (!ParseIntArg(arg.substr(14), &v)) return false;
      options->concurrency = std::max(1, v);
    } else if (arg.rfind("--output_dir=", 0) == 0) {
      options->output_dir = arg.substr(13);
    } else if (arg.rfind("--log=", 0) == 0) {
      // log level handled in main
    } else {
      std::cerr << "unknown arg: " << arg << "\n";
      return false;
    }
  }
  return true;
}

// ============================================================================
// RoundRobin 场景
// ============================================================================

PoolBenchmarkResult RunRoundRobinScenario(const BenchOptions& options) {
  constexpr std::uint16_t base_port = 50301;
  constexpr std::size_t kEndpointCount = 3;

  // 启动多个 server
  std::vector<std::unique_ptr<rpc::server::ServiceRegistry>> registries;
  std::vector<std::unique_ptr<rpc::server::RpcServer>> servers;
  std::vector<std::thread> server_threads;

  for (std::size_t i = 0; i < kEndpointCount; ++i) {
    auto registry = std::make_unique<rpc::server::ServiceRegistry>();
    (void)registry->Register(
        "BenchmarkService", "Echo",
        [i](std::string_view payload) { return std::string(payload); });

    // 保存 registry，确保生命周期
    registries.emplace_back(std::move(registry));

    // echo 服务很简单，1 worker + 0 business thread pool
    auto server = std::make_unique<rpc::server::RpcServer>(
        static_cast<std::uint16_t>(base_port + i), *registries.back(), 1, 0);
    auto* server_ptr = server.get();
    servers.emplace_back(std::move(server));
    
    // 每个服务器在独立线程中启动（Start 是阻塞的）
    server_threads.emplace_back([server_ptr]() {
      server_ptr->Start();
    });
  }

  // 等待所有 server 启动
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  for (std::size_t i = 0; i < kEndpointCount; ++i) {
    if (!WaitServerReady(static_cast<std::uint16_t>(base_port + i),
                         std::chrono::seconds(3))) {
      std::cerr << "server not ready on port " << (base_port + i) << "\n";
    }
  }

  // 创建 pool
  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", base_port},
       {"127.0.0.1", static_cast<std::uint16_t>(base_port + 1)},
       {"127.0.0.1", static_cast<std::uint16_t>(base_port + 2)}},
      rpc::client::RpcClientPoolOptions{
          .client_options = rpc::client::RpcClientOptions{
              .send_timeout = std::chrono::milliseconds(5000),
              .recv_timeout = std::chrono::milliseconds(5000),
              .heartbeat_interval = std::chrono::seconds(0),
              .heartbeat_timeout = std::chrono::seconds(0),
          },
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
      });

  pool.Warmup();

  std::string payload(static_cast<std::size_t>(options.payload_bytes), 'x');

  // 多线程发请求 - 每个线程使用独立的 latencies 向量，避免线程安全问题
  std::atomic<int> success_count{0};
  std::atomic<int> failed_count{0};
  std::atomic<int> timeout_count{0};

  const int base = options.total_requests / options.concurrency;
  const int rem = options.total_requests % options.concurrency;

  const auto begin = std::chrono::steady_clock::now();

  // 每个线程使用独立的 latencies 向量
  std::vector<std::vector<long long>> thread_latencies(
      static_cast<std::size_t>(options.concurrency));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(options.concurrency));
  for (int i = 0; i < options.concurrency; ++i) {
    const int req_count = base + (i < rem ? 1 : 0);
    thread_latencies[i].reserve(static_cast<std::size_t>(req_count));
    workers.emplace_back([&, i]() {
      for (int j = 0; j < req_count; ++j) {
        const auto t1 = std::chrono::steady_clock::now();
        auto result = pool.Call("BenchmarkService", "Echo", payload);
        const auto t2 = std::chrono::steady_clock::now();

        const auto latency =
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
                .count();

        thread_latencies[i].push_back(latency);

        if (result.ok() && result.response_payload == payload) {
          ++success_count;
        } else if (result.status.message.find("timeout") !=
                   std::string::npos) {
          ++timeout_count;
        } else {
          ++failed_count;
        }
      }
    });
  }

  for (auto& t : workers) {
    t.join();
  }

  // 合并所有线程的 latencies
  std::vector<long long> latencies;
  latencies.reserve(static_cast<std::size_t>(options.total_requests));
  for (const auto& tl : thread_latencies) {
    latencies.insert(latencies.end(), tl.begin(), tl.end());
  }

  const auto end = std::chrono::steady_clock::now();

  // 获取 pool stats
  auto endpoint_stats = pool.GetStats();

  // 停止 server
  for (auto& s : servers) {
    if (s) s->Stop();
  }
  for (auto& t : server_threads) {
    if (t.joinable()) t.join();
  }
  servers.clear();

  PoolBenchmarkResult result = BuildResult(
      "round_robin", options, success_count.load(), failed_count.load(),
      timeout_count.load(), begin, end);

  ComputeLatencyStats(latencies, &result);
  result.endpoint_stats = std::move(endpoint_stats);
  return result;
}

// ============================================================================
// LeastInflight 场景
// ============================================================================

PoolBenchmarkResult RunLeastInflightScenario(const BenchOptions& options) {
  constexpr std::uint16_t fast_port = 50401;
  constexpr std::uint16_t slow_port = 50402;

  // 启动两个 server：fast 和 slow
  // 使用向量保存 registry，确保生命周期
  std::vector<std::unique_ptr<rpc::server::ServiceRegistry>> registries;

  auto fast_registry = std::make_unique<rpc::server::ServiceRegistry>();
  (void)fast_registry->Register(
      "BenchmarkService", "Echo",
      [](std::string_view payload) { return std::string(payload); });
  auto* fast_registry_ptr = fast_registry.get();
  registries.emplace_back(std::move(fast_registry));

  auto slow_registry = std::make_unique<rpc::server::ServiceRegistry>();
  // slow server 故意 sleep 1ms
  (void)slow_registry->Register(
      "BenchmarkService", "Echo", [](std::string_view payload) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::string(payload);
      });
  auto* slow_registry_ptr = slow_registry.get();
  registries.emplace_back(std::move(slow_registry));

  // echo 服务很简单，减少不必要的线程
  auto fast_server = std::make_unique<rpc::server::RpcServer>(
      fast_port, *fast_registry_ptr, 1, 0);
  auto slow_server = std::make_unique<rpc::server::RpcServer>(
      slow_port, *slow_registry_ptr, 1, 0);

  auto* fast_server_ptr = fast_server.get();
  auto* slow_server_ptr = slow_server.get();

  // 在后台线程中启动服务器（Start 是阻塞的）
  std::thread fast_thread([fast_server_ptr]() { fast_server_ptr->Start(); });
  std::thread slow_thread([slow_server_ptr]() { slow_server_ptr->Start(); });

  WaitServerReady(fast_port, std::chrono::seconds(5));
  WaitServerReady(slow_port, std::chrono::seconds(5));

  // 创建 pool with LeastInflight
  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", fast_port}, {"127.0.0.1", slow_port}},
      rpc::client::RpcClientPoolOptions{
          .client_options = rpc::client::RpcClientOptions{
              .send_timeout = std::chrono::milliseconds(5000),
              .recv_timeout = std::chrono::milliseconds(5000),
              .heartbeat_interval = std::chrono::seconds(0),
              .heartbeat_timeout = std::chrono::seconds(0),
          },
          .strategy = rpc::client::LoadBalanceStrategy::kLeastInflight,
      });

  pool.Warmup();

  std::string payload(static_cast<std::size_t>(options.payload_bytes), 'x');

  // 使用 async 调用，让 inflight 更容易堆积
  std::atomic<int> success_count{0};
  std::atomic<int> failed_count{0};
  std::atomic<int> timeout_count{0};

  const int base = options.total_requests / options.concurrency;
  const int rem = options.total_requests % options.concurrency;

  const auto begin = std::chrono::steady_clock::now();

  // 每个线程使用独立的 latencies 向量，避免线程安全问题
  std::vector<std::vector<long long>> thread_latencies(
      static_cast<std::size_t>(options.concurrency));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(options.concurrency));
  for (int i = 0; i < options.concurrency; ++i) {
    const int req_count = base + (i < rem ? 1 : 0);
    thread_latencies[i].reserve(static_cast<std::size_t>(req_count));
    workers.emplace_back([&, i]() {
      for (int j = 0; j < req_count; ++j) {
        const auto t1 = std::chrono::steady_clock::now();
        auto future =
            pool.CallAsync("BenchmarkService", "Echo", payload);
        auto result = future.get();
        const auto t2 = std::chrono::steady_clock::now();

        const auto latency =
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
                .count();
        thread_latencies[i].push_back(latency);

        if (result.ok() && result.response_payload == payload) {
          ++success_count;
        } else if (result.status.message.find("timeout") !=
                   std::string::npos) {
          ++timeout_count;
        } else {
          ++failed_count;
        }
      }
    });
  }

  for (auto& t : workers) {
    t.join();
  }

  // 合并所有线程的 latencies
  std::vector<long long> latencies;
  latencies.reserve(static_cast<std::size_t>(options.total_requests));
  for (const auto& tl : thread_latencies) {
    latencies.insert(latencies.end(), tl.begin(), tl.end());
  }

  const auto end = std::chrono::steady_clock::now();

  auto endpoint_stats = pool.GetStats();

  // 停止 server
  if (slow_server) slow_server->Stop();
  if (fast_server) fast_server->Stop();
  if (slow_thread.joinable()) slow_thread.join();
  if (fast_thread.joinable()) fast_thread.join();
  slow_server.reset();
  fast_server.reset();

  PoolBenchmarkResult result = BuildResult(
      "least_inflight", options, success_count.load(), failed_count.load(),
      timeout_count.load(), begin, end);

  ComputeLatencyStats(latencies, &result);
  result.endpoint_stats = std::move(endpoint_stats);
  return result;
}

// ============================================================================
// Failover 场景
// ============================================================================

PoolBenchmarkResult RunFailoverScenario(const BenchOptions& options) {
  constexpr std::uint16_t healthy_port = 50501;
  constexpr std::uint16_t unhealthy_port = 50502;

  // 只启动 healthy server，不启动 unhealthy server
  // 使用向量保存 registry，确保生命周期
  std::vector<std::unique_ptr<rpc::server::ServiceRegistry>> registries;

  auto healthy_registry = std::make_unique<rpc::server::ServiceRegistry>();
  (void)healthy_registry->Register(
      "BenchmarkService", "Echo",
      [](std::string_view payload) { return std::string(payload); });
  auto* healthy_registry_ptr = healthy_registry.get();
  registries.emplace_back(std::move(healthy_registry));

  auto healthy_server = std::make_unique<rpc::server::RpcServer>(
      healthy_port, *healthy_registry_ptr, 1, 0);
  auto* healthy_server_ptr = healthy_server.get();
  // 在后台线程中启动服务器（Start 是阻塞的）
  std::thread healthy_thread([healthy_server_ptr]() { healthy_server_ptr->Start(); });
  
  WaitServerReady(healthy_port, std::chrono::seconds(5));

  // 创建 pool（包含不存在的 endpoint）
  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", unhealthy_port}, {"127.0.0.1", healthy_port}},
      rpc::client::RpcClientPoolOptions{
          .client_options = rpc::client::RpcClientOptions{
              .send_timeout = std::chrono::milliseconds(2000),
              .recv_timeout = std::chrono::milliseconds(2000),
              .heartbeat_interval = std::chrono::seconds(0),
              .heartbeat_timeout = std::chrono::seconds(0),
          },
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 3,
      });

  // Warmup 会标记 unhealthy endpoint
  pool.Warmup();

  std::string payload(static_cast<std::size_t>(options.payload_bytes), 'x');

  // 发送少量请求
  std::vector<long long> latencies;
  latencies.reserve(static_cast<std::size_t>(options.total_requests));
  std::atomic<int> success_count{0};
  std::atomic<int> failed_count{0};

  const auto begin = std::chrono::steady_clock::now();

  for (int i = 0; i < options.total_requests; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    auto result = pool.Call("BenchmarkService", "Echo", payload);
    const auto t2 = std::chrono::steady_clock::now();

    const auto latency =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
            .count();
    latencies.push_back(latency);

    if (result.ok() && result.response_payload == payload) {
      ++success_count;
    } else {
      ++failed_count;
    }
  }

  const auto end = std::chrono::steady_clock::now();

  auto endpoint_stats = pool.GetStats();

  // 停止 server
  if (healthy_server) healthy_server->Stop();
  if (healthy_thread.joinable()) healthy_thread.join();
  healthy_server.reset();

  PoolBenchmarkResult result = BuildResult(
      "failover", options, success_count.load(), failed_count.load(),
      0, begin, end);

  ComputeLatencyStats(latencies, &result);
  result.endpoint_stats = std::move(endpoint_stats);
  return result;
}

// ============================================================================
// 输出
// ============================================================================

void PrintPoolResult(const PoolBenchmarkResult& r) {
  std::cout << "\n=== RpcClientPool Benchmark: " << r.scenario << " ===\n";
  std::cout << "requests=" << r.total_requests
            << " payload=" << r.payload_bytes << " bytes\n";
  std::cout << "success=" << r.success_count
            << " failed=" << r.failed_count
            << " timeout=" << r.timeout_count << "\n\n";

  std::cout << "QPS=" << std::fixed << std::setprecision(0) << r.qps << "\n";
  std::cout << "Avg(us)=" << std::setprecision(1) << r.avg_latency_us << "\n";
  std::cout << "P95(us)=" << std::setprecision(1) << r.p95_latency_us << "\n";
  std::cout << "P99(us)=" << std::setprecision(1) << r.p99_latency_us << "\n";

  std::cout << "\nEndpoint stats:\n";
  std::cout << std::left << std::setw(22) << "endpoint" << std::right
            << std::setw(10) << "select" << std::setw(10) << "success"
            << std::setw(8) << "fail" << std::setw(10) << "inflight"
            << std::setw(10) << "healthy" << "\n";
  std::cout << std::string(70, '-') << "\n";

  for (const auto& ep : r.endpoint_stats) {
    std::cout << std::left << std::setw(22) << ep.endpoint << std::right
              << std::setw(10) << ep.select_count << std::setw(10)
              << ep.success_count << std::setw(8) << ep.fail_count
              << std::setw(10) << ep.inflight << std::setw(10)
              << (ep.healthy ? "true" : "false") << "\n";
  }

  // 计算分发比例
  std::size_t total_select = 0;
  for (const auto& ep : r.endpoint_stats) {
    total_select += ep.select_count;
  }
  if (total_select > 0) {
    std::cout << "\nDistribution:\n";
    for (const auto& ep : r.endpoint_stats) {
      double pct = 100.0 * ep.select_count / total_select;
      std::cout << "  " << ep.endpoint << ": " << std::fixed
                << std::setprecision(1) << pct << "%\n";
    }
  }
}

bool EnsureDirExists(const std::string& path) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(path), ec);
  return !ec;
}

bool WritePoolResult(const PoolBenchmarkResult& r,
                      const std::string& output_dir) {
  if (!EnsureDirExists(output_dir)) {
    return false;
  }

  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  const std::string file_stem = "rpc_client_pool_benchmark_" + r.scenario +
                                 "_r" + std::to_string(r.total_requests) +
                                 "_p" + std::to_string(r.payload_bytes) +
                                 "_" + std::to_string(now_ms);

  const std::string text_path = output_dir + "/" + file_stem + ".txt";
  std::ofstream tf(text_path);
  if (!tf) return false;

  tf << "=== RpcClientPool Benchmark: " << r.scenario << " ===\n";
  tf << "requests=" << r.total_requests
     << " payload=" << r.payload_bytes << " bytes\n\n";
  tf << "QPS=" << std::fixed << std::setprecision(0) << r.qps << "\n";
  tf << "Avg(us)=" << std::setprecision(1) << r.avg_latency_us << "\n";
  tf << "P95(us)=" << std::setprecision(1) << r.p95_latency_us << "\n";
  tf << "P99(us)=" << std::setprecision(1) << r.p99_latency_us << "\n\n";
  tf << "Endpoint stats:\n";
  tf << std::left << std::setw(22) << "endpoint" << std::right << std::setw(10)
     << "select" << std::setw(10) << "success" << std::setw(8) << "fail"
     << std::setw(10) << "inflight" << std::setw(10) << "healthy" << "\n";
  tf << std::string(70, '-') << "\n";

  for (const auto& ep : r.endpoint_stats) {
    tf << std::left << std::setw(22) << ep.endpoint << std::right
       << std::setw(10) << ep.select_count << std::setw(10) << ep.success_count
       << std::setw(8) << ep.fail_count << std::setw(10) << ep.inflight
       << std::setw(10) << (ep.healthy ? "true" : "false") << "\n";
  }
  tf.close();

  std::cout << "result_file=" << text_path << "\n";

  // 同时写入 summary.md
  const std::string summary_path = output_dir + "/summary.md";
  
  // 检查文件是否存在，如果不存在则添加标题
  bool file_exists = std::filesystem::exists(summary_path);
  
  std::ofstream sf(summary_path, std::ios::app);  // 追加模式
  if (sf) {
    if (!file_exists) {
      sf << "# RpcClientPool Benchmark Summary\n\n";
    }
    sf << "## Scenario: " << r.scenario << "\n\n";
    sf << "- **Requests**: " << r.total_requests << "\n";
    sf << "- **Payload**: " << r.payload_bytes << " bytes\n";
    sf << "- **QPS**: " << std::fixed << std::setprecision(0) << r.qps << "\n";
    sf << "- **Avg Latency**: " << std::setprecision(1) << r.avg_latency_us << " us\n";
    sf << "- **P95 Latency**: " << std::setprecision(1) << r.p95_latency_us << " us\n";
    sf << "- **P99 Latency**: " << std::setprecision(1) << r.p99_latency_us << " us\n";
    sf << "- **Success**: " << r.success_count << "\n";
    sf << "- **Failed**: " << r.failed_count << "\n";
    if (r.timeout_count > 0) {
      sf << "- **Timeout**: " << r.timeout_count << "\n";
    }
    sf << "\n### Endpoint Stats\n\n";
    sf << "| Endpoint | Select | Success | Fail | Inflight | Healthy |\n";
    sf << "|----------|--------|---------|------|----------|--------|\n";
    for (const auto& ep : r.endpoint_stats) {
      sf << "| " << ep.endpoint << " | " << ep.select_count << " | "
         << ep.success_count << " | " << ep.fail_count << " | "
         << ep.inflight << " | " << (ep.healthy ? "true" : "false") << " |\n";
    }
    sf << "\n---\n\n";
    sf.close();
    std::cout << "summary_file=" << summary_path << "\n";
  }

  return true;
}

void PrintUsage() {
  std::cout << "usage: rpc_client_pool_benchmark "
               "[--scenario=round_robin|least_inflight|failover] "
               "[--requests=N] [--payload_bytes=N] [--concurrency=N]\n"
            << "  --scenario=round_robin   测试 RoundRobin 分发 (默认)\n"
            << "  --scenario=least_inflight 测试 LeastInflight 策略\n"
            << "  --scenario=failover     测试故障转移\n"
            << "  --requests=N           总请求数 (默认 30000)\n"
            << "  --payload_bytes=N      payload 大小 (默认 64)\n"
            << "  --concurrency=N        并发数 (默认 8)\n";
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
  return rpc::common::LogLevel::kOff;
}

int main(int argc, char** argv) {
  rpc::common::SetLogLevel(ParseLogArg(argc, argv));

  BenchOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage();
    return 1;
  }

  PoolBenchmarkResult result;
  switch (options.scenario) {
    case BenchmarkScenario::kRoundRobin:
      result = RunRoundRobinScenario(options);
      break;
    case BenchmarkScenario::kLeastInflight:
      result = RunLeastInflightScenario(options);
      break;
    case BenchmarkScenario::kFailover:
      result = RunFailoverScenario(options);
      break;
  }

  PrintPoolResult(result);
  WritePoolResult(result, options.output_dir);

  std::cout << "\ndone.\n";
  return 0;
}
