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

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/rpc_error.h"

namespace {

constexpr std::uint16_t kTestPort = 50054;

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

int RunHoldServer() {
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

    // 忽略探活连接。
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 给真实连接留一段时间，让客户端发完请求后主动 Close。
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    _exit(RunHoldServer());
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "call_async_close_test failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", kTestPort,
      {.send_timeout = std::chrono::milliseconds(500),
       .recv_timeout = std::chrono::milliseconds(1500)});

  calc::AddRequest req;
  req.set_a(1);
  req.set_b(2);
  std::string payload;
  assert(req.SerializeToString(&payload));

  auto fut = client.CallAsync("CalcService", "Add", payload);
  client.Close();

  const auto result = fut.get();
  assert(!result.ok());
  assert(result.status.code ==
         rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError));
  assert(result.status.message == "connection closed" ||
         result.status.message.find("dispatcher stopped") != std::string::npos);

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  assert(WEXITSTATUS(child_status) == 0);

  std::cout << "call_async_close_test passed\n";
  return 0;
}
