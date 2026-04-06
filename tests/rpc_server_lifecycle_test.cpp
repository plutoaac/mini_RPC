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

bool WaitServerClosed(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (!CanConnect(port)) {
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

void TestRpcServerStartStopLifecycle() {
  constexpr std::uint16_t kPort = 50061;

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

  rpc::server::RpcServer server(kPort, registry, 2U);

  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });

  assert(WaitServerReady(kPort, std::chrono::seconds(5)));

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

  assert(server.Stop());
  server_thread.join();
  assert(start_result);
  assert(WaitServerClosed(kPort, std::chrono::seconds(2)));

  rpc::client::RpcClient after_stop(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(200),
       .recv_timeout = std::chrono::milliseconds(200)});

  const auto sync_after =
      after_stop.Call("CalcService", "Add", BuildAddPayload(7, 8));
  assert(!sync_after.ok());

  auto async_after_future =
      after_stop.CallAsync("CalcService", "Add", BuildAddPayload(9, 10));
  const auto async_after = async_after_future.get();
  assert(!async_after.ok());

  const auto co_after = rpc::coroutine::SyncWait(
      after_stop.CallCo("CalcService", "Add", BuildAddPayload(11, 12)));
  assert(!co_after.ok());
}

}  // namespace

int main() {
  TestRpcServerStartStopLifecycle();
  std::cout << "rpc_server_lifecycle_test passed\n";
  return 0;
}
