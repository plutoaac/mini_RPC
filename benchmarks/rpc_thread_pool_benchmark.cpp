#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "benchmark_stats.h"
#include "calc.pb.h"
#include "client/rpc_client.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

bool CanConnect(std::uint16_t port) {
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

bool WaitServerReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string BuildAddPayload(int a, int b) {
  calc::AddRequest req;
  req.set_a(a);
  req.set_b(b);
  std::string payload;
  const bool ok = req.SerializeToString(&payload);
  assert(ok);
  return payload;
}

bool IsTimeoutResult(const rpc::client::RpcCallResult& result) {
  return result.status.message.find("timeout") != std::string::npos;
}

rpc::benchmark::BenchmarkResult RunScenario(std::string name,
                                            std::uint16_t port,
                                            std::size_t worker_count,
                                            std::size_t business_thread_count,
                                            int total_requests, int concurrency,
                                            int handler_sleep_ms) {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register(
      "SlowService", "Add", [handler_sleep_ms](std::string_view in) {
        calc::AddRequest req;
        if (!req.ParseFromArray(in.data(), static_cast<int>(in.size()))) {
          throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                      "failed to parse request");
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(handler_sleep_ms));

        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        const bool ok = resp.SerializeToString(&out);
        assert(ok);
        return out;
      }));

  rpc::server::RpcServer server(port, registry, worker_count,
                                business_thread_count);

  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });

  if (!WaitServerReady(port, std::chrono::seconds(2))) {
    std::cerr << "server not ready for scenario " << name << "\n";
    std::abort();
  }

  rpc::benchmark::BenchmarkStats stats(
      std::move(name), concurrency,
      static_cast<int>(BuildAddPayload(1, 2).size()), total_requests);

  const int base = total_requests / concurrency;
  const int rem = total_requests % concurrency;

  std::vector<rpc::benchmark::LocalStats> locals(
      static_cast<std::size_t>(concurrency));
  const auto begin = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(concurrency));
  for (int i = 0; i < concurrency; ++i) {
    const int request_count = base + ((i < rem) ? 1 : 0);
    workers.emplace_back([&, i, request_count]() {
      rpc::client::RpcClient client(
          "127.0.0.1", port,
          {.send_timeout = std::chrono::milliseconds(2000),
           .recv_timeout = std::chrono::milliseconds(3000)});

      rpc::benchmark::LocalStats local;
      local.latency_us.reserve(static_cast<std::size_t>(request_count));

      for (int j = 0; j < request_count; ++j) {
        const int x = i * 100000 + j;
        const auto t1 = std::chrono::steady_clock::now();
        const auto result =
            client.Call("SlowService", "Add", BuildAddPayload(x, 1));
        const auto t2 = std::chrono::steady_clock::now();

        local.latency_us.push_back(
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
                .count());

        if (!result.ok()) {
          if (IsTimeoutResult(result)) {
            ++local.timeout_count;
          } else {
            ++local.failed_count;
          }
          continue;
        }

        calc::AddResponse resp;
        const bool parsed = resp.ParseFromString(result.response_payload);
        if (!parsed || resp.result() != x + 1) {
          ++local.failed_count;
          continue;
        }

        ++local.success_count;
      }

      locals[static_cast<std::size_t>(i)] = std::move(local);
    });
  }

  for (auto& t : workers) {
    t.join();
  }

  const auto end = std::chrono::steady_clock::now();

  for (auto& local : locals) {
    stats.Merge(std::move(local));
  }

  assert(server.Stop());
  server_thread.join();
  assert(start_result);

  return stats.Finalize(begin, end);
}

}  // namespace

int main(int argc, char** argv) {
  const int total_requests =
      (argc > 1) ? std::max(32, std::atoi(argv[1])) : 128;
  const int concurrency = (argc > 2) ? std::max(2, std::atoi(argv[2])) : 16;
  const int handler_sleep_ms =
      (argc > 3) ? std::max(1, std::atoi(argv[3])) : 20;
  const std::string output_dir =
      (argc > 4) ? std::string(argv[4]) : "benchmarks/results";

  const rpc::benchmark::BenchmarkResult inline_result =
      RunScenario("inline-handler", 50211, 2U, 0U, total_requests, concurrency,
                  handler_sleep_ms);
  const rpc::benchmark::BenchmarkResult pool_result =
      RunScenario("thread-pool-handler", 50212, 2U, 4U, total_requests,
                  concurrency, handler_sleep_ms);

  rpc::benchmark::PrintBenchmarkResult(inline_result);
  rpc::benchmark::PrintBenchmarkCsv(inline_result);
  rpc::benchmark::PrintBenchmarkResult(pool_result);
  rpc::benchmark::PrintBenchmarkCsv(pool_result);

  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  std::string text_path;
  std::string csv_path;
  const std::string inline_stem =
      "rpc_thread_pool_benchmark_inline_c" + std::to_string(concurrency) +
      "_r" + std::to_string(total_requests) + "_sleep" +
      std::to_string(handler_sleep_ms) + "_" + std::to_string(now_ms);
  if (rpc::benchmark::WriteBenchmarkResultFiles(
          inline_result, output_dir, inline_stem, &text_path, &csv_path)) {
    std::cout << "inline_result_text_file=" << text_path << '\n';
    std::cout << "inline_result_csv_file=" << csv_path << '\n';
  }

  const std::string pool_stem =
      "rpc_thread_pool_benchmark_pool_c" + std::to_string(concurrency) + "_r" +
      std::to_string(total_requests) + "_sleep" +
      std::to_string(handler_sleep_ms) + "_" + std::to_string(now_ms);
  if (rpc::benchmark::WriteBenchmarkResultFiles(
          pool_result, output_dir, pool_stem, &text_path, &csv_path)) {
    std::cout << "pool_result_text_file=" << text_path << '\n';
    std::cout << "pool_result_csv_file=" << csv_path << '\n';
  }

  const double speedup = pool_result.qps / inline_result.qps;
  std::cout << "qps_speedup=" << speedup << "x\n";

  return 0;
}
