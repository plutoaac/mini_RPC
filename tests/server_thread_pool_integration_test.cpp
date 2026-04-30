#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "coroutine/task.h"
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

int ParseAddResult(const std::string& payload) {
  calc::AddResponse resp;
  const bool ok = resp.ParseFromString(payload);
  assert(ok);
  return resp.result();
}

bool ContainsMethodStats(const rpc::server::RpcServer::RuntimeStatsSnapshot& stats,
                         const std::string& method,
                         std::size_t minimum_calls) {
  std::size_t observed_calls = 0;
  for (const auto& worker : stats.workers) {
    for (const auto& entry : worker.method_stats) {
      if (entry.method == method) {
        observed_calls += entry.call_count;
      }
    }
  }
  return observed_calls >= minimum_calls;
}

void TestBasicCallRegressionWithBusinessThreadPool() {
  constexpr std::uint16_t kPort = 50111;

  rpc::server::ServiceRegistry registry;
  assert(registry.Register("CalcService", "Add", [](std::string_view in) {
    calc::AddRequest req;
    if (!req.ParseFromArray(in.data(), static_cast<int>(in.size()))) {
      throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                  "failed to parse add request");
    }

    calc::AddResponse resp;
    resp.set_result(req.a() + req.b());

    std::string out;
    const bool ok = resp.SerializeToString(&out);
    assert(ok);
    return out;
  }));

  rpc::server::RpcServer server(kPort, registry, 2U, 4U);
  auto start_result = std::make_shared<bool>(false);
  std::thread server_thread([start_result, &server]() {
    *start_result = server.Start();
  });

  assert(WaitServerReady(kPort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc::client::RpcClient client(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(1000)});

  const auto sync_res = client.Call("CalcService", "Add", BuildAddPayload(1, 2));
  assert(sync_res.ok());
  assert(ParseAddResult(sync_res.response_payload) == 3);

  auto future = client.CallAsync("CalcService", "Add", BuildAddPayload(3, 4));
  const auto async_res = future.get();
  assert(async_res.ok());
  assert(ParseAddResult(async_res.response_payload) == 7);

  const auto co_res = rpc::coroutine::SyncWait(
      client.CallCo("CalcService", "Add", BuildAddPayload(5, 6)));
  assert(co_res.ok());
  assert(ParseAddResult(co_res.response_payload) == 11);

  const auto stats = server.StatsSnapshot();
  assert(stats.business_thread_pool.has_value());
  assert(stats.business_thread_pool->submitted_tasks >= 3U);
  assert(ContainsMethodStats(stats, "CalcService.Add", 3U));

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  assert(*start_result);
}

void TestSlowHandlerConcurrencyAndStats() {
  constexpr std::uint16_t kPort = 50112;
  constexpr int kRequests = 24;

  rpc::server::ServiceRegistry registry;
  assert(registry.Register("SlowService", "Add", [](std::string_view in) {
    calc::AddRequest req;
    if (!req.ParseFromArray(in.data(), static_cast<int>(in.size()))) {
      throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                  "failed to parse add request");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    calc::AddResponse resp;
    resp.set_result(req.a() + req.b());
    std::string out;
    const bool ok = resp.SerializeToString(&out);
    assert(ok);
    return out;
  }));

  rpc::server::RpcServer server(kPort, registry, 2U, 4U);
  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });

  assert(WaitServerReady(kPort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  rpc::client::RpcClient client(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(2000)});

  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  futures.reserve(kRequests);
  for (int i = 0; i < kRequests; ++i) {
    futures.push_back(
        client.CallAsync("SlowService", "Add", BuildAddPayload(i, i + 1)));
  }

  for (int i = 0; i < kRequests; ++i) {
    const auto result = futures[static_cast<std::size_t>(i)].get();
    assert(result.ok());
    assert(ParseAddResult(result.response_payload) == (2 * i + 1));
  }

  const auto stats = server.StatsSnapshot();
  assert(stats.business_thread_pool.has_value());
  assert(stats.business_thread_pool->submitted_tasks >=
         static_cast<std::size_t>(kRequests));
  assert(stats.business_thread_pool->completed_tasks >=
         static_cast<std::size_t>(kRequests));
  assert(ContainsMethodStats(stats, "SlowService.Add",
                              static_cast<std::size_t>(kRequests)));

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  assert(start_result);
}

void TestCloseRaceDropsCompletedResponsesSafely() {
  constexpr std::uint16_t kPort = 50113;
  constexpr int kRequests = 16;

  rpc::server::ServiceRegistry registry;
  assert(registry.Register("SlowService", "Add", [](std::string_view in) {
    calc::AddRequest req;
    if (!req.ParseFromArray(in.data(), static_cast<int>(in.size()))) {
      throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                  "failed to parse add request");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    calc::AddResponse resp;
    resp.set_result(req.a() + req.b());
    std::string out;
    const bool ok = resp.SerializeToString(&out);
    assert(ok);
    return out;
  }));

  rpc::server::RpcServer server(kPort, registry, 2U, 4U);
  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });

  assert(WaitServerReady(kPort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  {
    rpc::client::RpcClient client(
        "127.0.0.1", kPort,
        {.send_timeout = std::chrono::milliseconds(1000),
         .recv_timeout = std::chrono::milliseconds(500)});

    futures.reserve(kRequests);
    for (int i = 0; i < kRequests; ++i) {
      futures.push_back(client.CallAsync("SlowService", "Add",
                                         BuildAddPayload(i, i + 2)));
    }

    client.Close();
  }

  for (auto& future : futures) {
    const auto status = future.wait_for(std::chrono::seconds(2));
    assert(status == std::future_status::ready);
    (void)future.get();
  }

  rpc::client::RpcClient probe(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(1500)});
  const auto probe_result =
      probe.Call("SlowService", "Add", BuildAddPayload(10, 20));
  assert(probe_result.ok());
  assert(ParseAddResult(probe_result.response_payload) == 30);

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  assert(start_result);
}

}  // namespace

int main() {
  TestBasicCallRegressionWithBusinessThreadPool();
  TestSlowHandlerConcurrencyAndStats();
  TestCloseRaceDropsCompletedResponsesSafely();
  std::cout << "server_thread_pool_integration_test passed\n";
  return 0;
}
