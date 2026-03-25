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
      // Ignore transient probe errors until timeout.
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
    std::cerr << "call_co_basic_test failed: server not ready\n";
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

  const auto add_res = rpc::coroutine::SyncWait(
      client.CallCo("CalcService", "Add", add_payload));
  assert(add_res.ok());

  calc::AddResponse add_resp;
  assert(add_resp.ParseFromString(add_res.response_payload));
  assert(add_resp.result() == 3);

  constexpr int kConcurrent = 16;
  std::vector<rpc::coroutine::Task<rpc::client::RpcCallResult>> tasks;
  tasks.reserve(kConcurrent);

  for (int i = 0; i < kConcurrent; ++i) {
    calc::AddRequest req;
    req.set_a(i);
    req.set_b(100);

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
    assert(resp.result() == i + 100);
  }

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  std::cout << "call_co_basic_test passed\n";
  return 0;
}
