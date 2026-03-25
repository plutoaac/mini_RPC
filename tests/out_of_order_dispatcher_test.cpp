#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace {

constexpr std::uint16_t kTestPort = 50052;
constexpr int kConcurrentCalls = 16;

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
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
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
    requests.reserve(kConcurrentCalls);

    bool probe_connection = false;
    for (int i = 0; i < kConcurrentCalls; ++i) {
      rpc::RpcRequest req;
      std::string read_error;
      if (!rpc::protocol::Codec::ReadMessage(conn_fd, &req, &read_error)) {
        // WaitServerReady() 会建立一次探活连接并立即关闭，这里应忽略该连接。
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

    // 明确乱序：按接收顺序的逆序返回响应。
    for (int i = kConcurrentCalls - 1; i >= 0; --i) {
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

      std::this_thread::sleep_for(std::chrono::milliseconds(5));

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
    std::cerr << "out_of_order_dispatcher_test failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", kTestPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(2000)});

  std::vector<int> results(static_cast<std::size_t>(kConcurrentCalls), -1);
  std::vector<std::thread> workers;
  workers.reserve(kConcurrentCalls);

  std::mutex finish_mu;
  std::vector<int> finish_order;
  finish_order.reserve(kConcurrentCalls);

  for (int i = 0; i < kConcurrentCalls; ++i) {
    workers.emplace_back([&client, &results, &finish_mu, &finish_order, i]() {
      calc::AddRequest req;
      req.set_a(i);
      req.set_b(1000);

      std::string payload;
      assert(req.SerializeToString(&payload));

      const auto res = client.Call("CalcService", "Add", payload);
      assert(res.ok());

      calc::AddResponse resp;
      assert(resp.ParseFromString(res.response_payload));
      results[static_cast<std::size_t>(i)] = resp.result();

      std::scoped_lock lock(finish_mu);
      finish_order.push_back(i);
    });
  }

  for (auto& worker : workers) {
    worker.join();
  }

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  assert(WEXITSTATUS(child_status) == 0);

  for (int i = 0; i < kConcurrentCalls; ++i) {
    assert(results[static_cast<std::size_t>(i)] == i + 1000);
  }

  bool strictly_increasing_finish = true;
  for (std::size_t i = 1; i < finish_order.size(); ++i) {
    if (finish_order[i] < finish_order[i - 1]) {
      strictly_increasing_finish = false;
      break;
    }
  }

  // 在模拟乱序服务端下，完成顺序通常不会严格递增；
  // 若偶发递增，不影响核心正确性验证（request_id 精确匹配）。
  if (strictly_increasing_finish) {
    std::cerr
        << "warning: completion order happened to be increasing in this run\n";
  }

  std::cout << "out_of_order_dispatcher_test passed\n";
  return 0;
}
