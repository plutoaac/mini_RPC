#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cerrno>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "coroutine/task.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace {

constexpr std::uint16_t kTestPort = 50065;
constexpr int kRequestCount = 20;

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

int RunOutOfOrderServer() {
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

  while (true) {
    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      ::close(listen_fd);
      return 6;
    }

    std::vector<rpc::RpcRequest> requests;
    requests.reserve(kRequestCount);

    bool probe_connection = false;
    for (int i = 0; i < kRequestCount; ++i) {
      rpc::RpcRequest req;
      std::string read_error;
      if (!rpc::protocol::Codec::ReadMessage(conn_fd, &req, &read_error)) {
        if (i == 0 && read_error == "peer closed connection") {
          probe_connection = true;
          break;
        }
        ::close(conn_fd);
        ::close(listen_fd);
        return 7;
      }
      requests.push_back(std::move(req));
    }

    if (probe_connection) {
      ::close(conn_fd);
      continue;
    }

    for (int i = kRequestCount - 1; i >= 0; --i) {
      calc::AddRequest add_req;
      if (!add_req.ParseFromString(
              requests[static_cast<std::size_t>(i)].payload())) {
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
      response.set_request_id(
          requests[static_cast<std::size_t>(i)].request_id());
      response.set_error_code(rpc::OK);
      response.set_payload(payload);

      std::this_thread::sleep_for(std::chrono::milliseconds(3));

      std::string write_error;
      if (!rpc::protocol::Codec::WriteMessage(conn_fd, response,
                                              &write_error)) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 10;
      }
    }

    ::close(conn_fd);
    ::close(listen_fd);
    return 0;
  }
}

}  // namespace

int main() {
  const pid_t pid = ::fork();
  assert(pid >= 0);

  if (pid == 0) {
    _exit(RunOutOfOrderServer());
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "call_mixed_waiters_test failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", kTestPort,
      {.send_timeout = std::chrono::milliseconds(800),
       .recv_timeout = std::chrono::milliseconds(1200)});

  std::vector<int> expected(static_cast<std::size_t>(kRequestCount), -1);
  std::vector<int> actual(static_cast<std::size_t>(kRequestCount), -1);

  std::vector<std::pair<int, std::future<rpc::client::RpcCallResult>>>
      async_waiters;
  std::vector<std::pair<int, rpc::coroutine::Task<rpc::client::RpcCallResult>>>
      coroutine_waiters;

  async_waiters.reserve(kRequestCount / 2 + 1);
  coroutine_waiters.reserve(kRequestCount / 2 + 1);

  for (int i = 0; i < kRequestCount; ++i) {
    calc::AddRequest req;
    req.set_a(i);
    req.set_b(500);

    std::string payload;
    assert(req.SerializeToString(&payload));
    expected[static_cast<std::size_t>(i)] = i + 500;

    if ((i % 2) == 0) {
      async_waiters.emplace_back(
          i, client.CallAsync("CalcService", "Add", payload));
    } else {
      coroutine_waiters.emplace_back(
          i, client.CallCo("CalcService", "Add", payload));
    }
  }

  for (auto& waiter : async_waiters) {
    const auto res = waiter.second.get();
    assert(res.ok());

    calc::AddResponse parsed;
    assert(parsed.ParseFromString(res.response_payload));
    actual[static_cast<std::size_t>(waiter.first)] = parsed.result();
  }

  for (auto& waiter : coroutine_waiters) {
    const auto res = rpc::coroutine::SyncWait(std::move(waiter.second));
    assert(res.ok());

    calc::AddResponse parsed;
    assert(parsed.ParseFromString(res.response_payload));
    actual[static_cast<std::size_t>(waiter.first)] = parsed.result();
  }

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  assert(WEXITSTATUS(child_status) == 0);

  for (int i = 0; i < kRequestCount; ++i) {
    assert(actual[static_cast<std::size_t>(i)] ==
           expected[static_cast<std::size_t>(i)]);
  }

  std::cout << "call_mixed_waiters_test passed\n";
  return 0;
}
