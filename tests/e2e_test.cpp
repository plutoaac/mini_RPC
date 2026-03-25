#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/rpc_error.h"

namespace {

bool WaitServerReady(const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();

  while (std::chrono::steady_clock::now() - start < timeout) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(50051);
      (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
          0) {
        ::close(fd);
        return true;
      }
      ::close(fd);
    }

    if (errno != ECONNREFUSED && errno != ETIMEDOUT && errno != EINPROGRESS) {
      // 对未知错误不立即失败，继续等待直到超时，避免瞬态问题。
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

int main() {
  const pid_t pid = ::fork();
  assert(pid >= 0);

  if (pid == 0) {
    ::execl("./rpc_server_demo", "./rpc_server_demo", nullptr);
    _exit(127);
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "e2e_test failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", 50051,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(1000)});

  calc::AddRequest add_req;
  add_req.set_a(1);
  add_req.set_b(2);

  std::string add_payload;
  assert(add_req.SerializeToString(&add_payload));

  const auto add_res = client.Call("CalcService", "Add", add_payload);
  assert(add_res.ok());

  calc::AddResponse add_resp;
  assert(add_resp.ParseFromString(add_res.response_payload));
  assert(add_resp.result() == 3);

  // 异步调用基本成功。
  auto add_future = client.CallAsync("CalcService", "Add", add_payload);
  const auto add_async_res = add_future.get();
  assert(add_async_res.ok());

  calc::AddResponse add_async_resp;
  assert(add_async_resp.ParseFromString(add_async_res.response_payload));
  assert(add_async_resp.result() == 3);

  const auto missing_res =
      client.Call("CalcService", "NoSuchMethod", add_payload);
  assert(!missing_res.ok());
  assert(missing_res.status.code ==
         rpc::common::make_error_code(rpc::common::ErrorCode::kMethodNotFound));

  // 并发调用验证：单连接多请求 in-flight 场景。
  constexpr int kConcurrent = 16;
  std::vector<std::thread> workers;
  workers.reserve(kConcurrent);

  for (int i = 0; i < kConcurrent; ++i) {
    workers.emplace_back([&client, i]() {
      calc::AddRequest req;
      req.set_a(i);
      req.set_b(i + 1);

      std::string payload;
      assert(req.SerializeToString(&payload));

      const auto res = client.Call("CalcService", "Add", payload);
      assert(res.ok());

      calc::AddResponse resp;
      assert(resp.ParseFromString(res.response_payload));
      assert(resp.result() == i + (i + 1));
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  // 并发异步调用验证：先全部发起，再统一 get。
  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  futures.reserve(kConcurrent);
  for (int i = 0; i < kConcurrent; ++i) {
    calc::AddRequest req;
    req.set_a(i);
    req.set_b(100);

    std::string payload;
    assert(req.SerializeToString(&payload));
    futures.emplace_back(client.CallAsync("CalcService", "Add", payload));
  }

  for (int i = 0; i < kConcurrent; ++i) {
    const auto res = futures[static_cast<std::size_t>(i)].get();
    assert(res.ok());

    calc::AddResponse resp;
    assert(resp.ParseFromString(res.response_payload));
    assert(resp.result() == i + 100);
  }

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  std::cout << "e2e_test passed\n";
  return 0;
}
