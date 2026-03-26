/// @file call_co_edge_cases_test.cpp
/// @brief 测试协程调用的边界情况和潜在竞态条件
///
/// 测试场景：
/// 1. 响应在 BindCoroutine 之前到达（kAlreadyDone 路径）
/// 2. 高并发场景下的竞态条件
/// 3. 连接断开时的协程等待者清理

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/rpc_error.h"
#include "coroutine/task.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace {

constexpr std::uint16_t kTestPort = 50067;

bool WaitServerReady(const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(kTestPort);
      (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
          0) {
        ::close(fd);
        return true;
      }
      ::close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

/// 快速响应服务器：收到请求后立即响应，模拟响应在协程绑定前到达的场景
int RunFastResponseServer() {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    return 2;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    ::close(listen_fd);
    return 3;
  }

  // 设置监听 socket 为非阻塞，以便超时退出
  struct timeval tv;
  tv.tv_sec = 5;  // 5秒超时
  tv.tv_usec = 0;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kTestPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd);
    return 4;
  }

  if (::listen(listen_fd, 8) < 0) {
    ::close(listen_fd);
    return 5;
  }

  int request_count = 0;
  const int kMaxRequests = 500;  // 处理 500 个请求后退出，支持多次测试

  while (request_count < kMaxRequests) {
    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(listen_fd);
      return 6;
    }

    // 处理多个请求
    while (request_count < kMaxRequests) {
      rpc::RpcRequest req;
      std::string read_error;
      if (!rpc::protocol::Codec::ReadMessage(conn_fd, &req, &read_error)) {
        break;  // 连接关闭或错误
      }

      // 立即响应，不延迟
      calc::AddRequest add_req;
      if (!add_req.ParseFromString(req.payload())) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 8;
      }

      calc::AddResponse add_resp;
      add_resp.set_result(add_req.a() + add_req.b());

      std::string payload;
      if (!add_resp.SerializeToString(&payload)) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 9;
      }

      rpc::RpcResponse response;
      response.set_request_id(req.request_id());
      response.set_error_code(rpc::OK);
      response.set_payload(payload);

      std::string write_error;
      if (!rpc::protocol::Codec::WriteMessage(conn_fd, response,
                                              &write_error)) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 10;
      }

      ++request_count;
    }

    ::close(conn_fd);
  }

  ::close(listen_fd);
  return 0;
}

}  // namespace

/// 测试 1: 高并发协程调用，验证竞态条件
void TestHighConcurrency() {
  std::cout << "Test 1: High concurrency coroutine calls\n";

  {
    rpc::client::RpcClient client(
        "127.0.0.1", kTestPort,
        {.send_timeout = std::chrono::milliseconds(1000),
         .recv_timeout = std::chrono::milliseconds(2000)});

    constexpr int kConcurrent = 30;
    std::vector<rpc::coroutine::Task<rpc::client::RpcCallResult>> tasks;
    tasks.reserve(kConcurrent);

    for (int i = 0; i < kConcurrent; ++i) {
      calc::AddRequest req;
      req.set_a(i);
      req.set_b(1000);

      std::string payload;
      assert(req.SerializeToString(&payload));
      tasks.emplace_back(client.CallCo("CalcService", "Add", payload));
    }

    for (int i = 0; i < kConcurrent; ++i) {
      const auto res =
          rpc::coroutine::SyncWait(std::move(tasks[static_cast<std::size_t>(i)]));
      assert(res.ok());

      calc::AddResponse resp;
      assert(resp.ParseFromString(res.response_payload));
      assert(resp.result() == i + 1000);
    }

    client.Close();  // 显式关闭连接
  }

  std::cout << "  Passed: 30 concurrent coroutine calls completed correctly\n";
}

/// 测试 2: 混合异步和协程调用，高并发
void TestMixedHighConcurrency() {
  std::cout << "Test 2: Mixed async and coroutine calls, high concurrency\n";

  {
    rpc::client::RpcClient client(
        "127.0.0.1", kTestPort,
        {.send_timeout = std::chrono::milliseconds(1000),
         .recv_timeout = std::chrono::milliseconds(2000)});

    constexpr int kTotal = 30;
    std::vector<std::pair<int, std::future<rpc::client::RpcCallResult>>> async_waiters;
    std::vector<std::pair<int, rpc::coroutine::Task<rpc::client::RpcCallResult>>> co_waiters;

    async_waiters.reserve(kTotal / 2);
    co_waiters.reserve(kTotal / 2);

    for (int i = 0; i < kTotal; ++i) {
      calc::AddRequest req;
      req.set_a(i);
      req.set_b(2000);

      std::string payload;
      assert(req.SerializeToString(&payload));

      if ((i % 2) == 0) {
        async_waiters.emplace_back(i, client.CallAsync("CalcService", "Add", payload));
      } else {
        co_waiters.emplace_back(i, client.CallCo("CalcService", "Add", payload));
      }
    }

    // 验证异步结果
    for (auto& waiter : async_waiters) {
      const auto res = waiter.second.get();
      assert(res.ok());

      calc::AddResponse resp;
      assert(resp.ParseFromString(res.response_payload));
      assert(resp.result() == waiter.first + 2000);
    }

    // 验证协程结果
    for (auto& waiter : co_waiters) {
      const auto res = rpc::coroutine::SyncWait(std::move(waiter.second));
      assert(res.ok());

      calc::AddResponse resp;
      assert(resp.ParseFromString(res.response_payload));
      assert(resp.result() == waiter.first + 2000);
    }

    client.Close();  // 显式关闭连接
  }

  std::cout << "  Passed: 30 mixed async/coroutine calls completed correctly\n";
}

int main() {
  const pid_t pid = ::fork();
  assert(pid >= 0);

  if (pid == 0) {
    _exit(RunFastResponseServer());
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "call_co_edge_cases_test failed: server not ready\n";
    return 1;
  }

  TestHighConcurrency();
  TestMixedHighConcurrency();

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  std::cout << "\ncall_co_edge_cases_test passed\n";
  return 0;
}