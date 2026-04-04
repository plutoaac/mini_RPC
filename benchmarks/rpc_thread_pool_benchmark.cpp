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

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

struct ScenarioResult {
  std::string name;
  int total_requests{0};
  int concurrency{0};
  double elapsed_ms{0.0};
  double avg_latency_us{0.0};
  double p95_latency_us{0.0};
  double qps{0.0};
};

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

ScenarioResult RunScenario(std::string name, std::uint16_t port,
                           std::size_t worker_count,
                           std::size_t business_thread_count,
                           int total_requests, int concurrency,
                           int handler_sleep_ms) {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("SlowService", "Add",
                           [handler_sleep_ms](std::string_view in) {
                             calc::AddRequest req;
                             if (!req.ParseFromArray(
                                     in.data(), static_cast<int>(in.size()))) {
                               throw rpc::server::RpcError(
                                   rpc::server::RpcStatusCode::kParseError,
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

  std::vector<long long> all_lat_us;
  all_lat_us.reserve(static_cast<std::size_t>(total_requests));
  std::mutex lat_mu;

  const int base = total_requests / concurrency;
  const int rem = total_requests % concurrency;

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

      std::vector<long long> local_lat;
      local_lat.reserve(static_cast<std::size_t>(request_count));

      for (int j = 0; j < request_count; ++j) {
        const int x = i * 100000 + j;
        const auto t1 = std::chrono::steady_clock::now();
        const auto result =
            client.Call("SlowService", "Add", BuildAddPayload(x, 1));
        const auto t2 = std::chrono::steady_clock::now();

        if (!result.ok()) {
          std::cerr << "rpc failed in scenario " << name
                    << ", code=" << result.status.code.value()
                    << ", msg=" << result.status.message << "\n";
          std::abort();
        }

        calc::AddResponse resp;
        const bool parsed = resp.ParseFromString(result.response_payload);
        if (!parsed || resp.result() != x + 1) {
          std::cerr << "invalid response in scenario " << name << "\n";
          std::abort();
        }

        local_lat.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                t2 - t1)
                                .count());
      }

      std::lock_guard<std::mutex> lock(lat_mu);
      all_lat_us.insert(all_lat_us.end(), local_lat.begin(), local_lat.end());
    });
  }

  for (auto& t : workers) {
    t.join();
  }

  const auto end = std::chrono::steady_clock::now();
  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();

  assert(server.Stop());
  server_thread.join();
  assert(start_result);

  std::sort(all_lat_us.begin(), all_lat_us.end());
  const long long total_lat_us =
      std::accumulate(all_lat_us.begin(), all_lat_us.end(), 0LL);

  ScenarioResult result;
  result.name = std::move(name);
  result.total_requests = total_requests;
  result.concurrency = concurrency;
  result.elapsed_ms = static_cast<double>(elapsed_us) / 1000.0;
  result.avg_latency_us =
      static_cast<double>(total_lat_us) / static_cast<double>(all_lat_us.size());
  result.p95_latency_us =
      static_cast<double>(all_lat_us[static_cast<std::size_t>(all_lat_us.size() * 0.95)]);
  result.qps = (1e6 * static_cast<double>(total_requests)) /
               static_cast<double>(elapsed_us);
  return result;
}

void PrintScenario(const ScenarioResult& result) {
  std::cout << "scenario=" << result.name << '\n';
  std::cout << "  requests=" << result.total_requests << '\n';
  std::cout << "  concurrency=" << result.concurrency << '\n';
  std::cout << "  elapsed_ms=" << result.elapsed_ms << '\n';
  std::cout << "  avg_latency_us=" << result.avg_latency_us << '\n';
  std::cout << "  p95_latency_us=" << result.p95_latency_us << '\n';
  std::cout << "  qps=" << result.qps << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  const int total_requests = (argc > 1) ? std::max(32, std::atoi(argv[1])) : 128;
  const int concurrency = (argc > 2) ? std::max(2, std::atoi(argv[2])) : 16;
  const int handler_sleep_ms = (argc > 3) ? std::max(1, std::atoi(argv[3])) : 20;

  const ScenarioResult inline_result =
      RunScenario("inline-handler", 50211, 2U, 0U, total_requests,
                  concurrency, handler_sleep_ms);
  const ScenarioResult pool_result =
      RunScenario("thread-pool-handler", 50212, 2U, 4U, total_requests,
                  concurrency, handler_sleep_ms);

  PrintScenario(inline_result);
  PrintScenario(pool_result);

  const double speedup = pool_result.qps / inline_result.qps;
  std::cout << "qps_speedup=" << speedup << "x\n";

  return 0;
}
