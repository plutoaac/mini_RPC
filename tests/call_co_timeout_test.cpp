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

constexpr std::uint16_t kTestPort = 50063;
constexpr int kRequestCount = 3;
constexpr int kSlowA = 123;

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

bool WriteAddResponse(const rpc::RpcRequest& request, int sleep_ms,
                      std::string* error_msg, int conn_fd) {
  calc::AddRequest add_req;
  if (!add_req.ParseFromString(request.payload())) {
    if (error_msg != nullptr) {
      *error_msg = "failed to parse AddRequest";
    }
    return false;
  }

  calc::AddResponse add_resp;
  add_resp.set_result(add_req.a() + add_req.b());

  std::string payload;
  if (!add_resp.SerializeToString(&payload)) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize AddResponse";
    }
    return false;
  }

  if (sleep_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }

  rpc::RpcResponse response;
  response.set_request_id(request.request_id());
  response.set_error_code(rpc::OK);
  response.set_payload(payload);

  return rpc::protocol::Codec::WriteMessage(conn_fd, response, error_msg);
}

int RunTimeoutServer() {
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

    for (const auto& req : requests) {
      calc::AddRequest parsed;
      if (!parsed.ParseFromString(req.payload())) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 8;
      }

      if (parsed.a() == kSlowA) {
        continue;
      }

      std::string write_error;
      if (!WriteAddResponse(req, 5, &write_error, conn_fd)) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 9;
      }
    }

    for (const auto& req : requests) {
      calc::AddRequest parsed;
      if (!parsed.ParseFromString(req.payload())) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 10;
      }
      if (parsed.a() != kSlowA) {
        continue;
      }

      std::string write_error;
      if (!WriteAddResponse(req, 350, &write_error, conn_fd)) {
        ::close(conn_fd);
        ::close(listen_fd);
        return 11;
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
    _exit(RunTimeoutServer());
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "call_co_timeout_test failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", kTestPort,
      {.send_timeout = std::chrono::milliseconds(800),
       .recv_timeout = std::chrono::milliseconds(120)});

  calc::AddRequest fast1;
  fast1.set_a(10);
  fast1.set_b(1);

  calc::AddRequest slow;
  slow.set_a(kSlowA);
  slow.set_b(7);

  calc::AddRequest fast2;
  fast2.set_a(20);
  fast2.set_b(3);

  std::string fast1_payload;
  std::string slow_payload;
  std::string fast2_payload;
  assert(fast1.SerializeToString(&fast1_payload));
  assert(slow.SerializeToString(&slow_payload));
  assert(fast2.SerializeToString(&fast2_payload));

  auto task_fast1 = client.CallCo("CalcService", "Add", fast1_payload);
  auto task_slow = client.CallCo("CalcService", "Add", slow_payload);
  auto task_fast2 = client.CallCo("CalcService", "Add", fast2_payload);

  const auto fast1_res = rpc::coroutine::SyncWait(std::move(task_fast1));
  const auto fast2_res = rpc::coroutine::SyncWait(std::move(task_fast2));
  const auto slow_res = rpc::coroutine::SyncWait(std::move(task_slow));

  assert(fast1_res.ok());
  assert(fast2_res.ok());

  calc::AddResponse fast1_resp;
  calc::AddResponse fast2_resp;
  assert(fast1_resp.ParseFromString(fast1_res.response_payload));
  assert(fast2_resp.ParseFromString(fast2_res.response_payload));
  assert(fast1_resp.result() == 11);
  assert(fast2_resp.result() == 23);

  assert(!slow_res.ok());
  assert(slow_res.status.code ==
         rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError));
  assert(slow_res.status.message == "call timeout");

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  assert(WEXITSTATUS(child_status) == 0);

  std::cout << "call_co_timeout_test passed\n";
  return 0;
}
