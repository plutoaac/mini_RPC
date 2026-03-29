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

      if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        ::close(fd);
        return true;
      }
      ::close(fd);
    }

    if (errno != ECONNREFUSED && errno != ETIMEDOUT && errno != EINPROGRESS) {
      // 保持重试直到超时，避免瞬态启动抖动。
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
    std::cerr << "server_multi_connection_test failed: server not ready\n";
    return 1;
  }

  constexpr int kClientCount = 8;
  constexpr int kCallsPerClient = 20;

  std::vector<std::thread> clients;
  clients.reserve(kClientCount);

  for (int i = 0; i < kClientCount; ++i) {
    clients.emplace_back([i]() {
      rpc::client::RpcClient client(
          "127.0.0.1", 50051,
          {.send_timeout = std::chrono::milliseconds(1000),
           .recv_timeout = std::chrono::milliseconds(1000)});

      for (int j = 0; j < kCallsPerClient; ++j) {
        calc::AddRequest req;
        req.set_a(i);
        req.set_b(j);

        std::string payload;
        assert(req.SerializeToString(&payload));

        const auto res = client.Call("CalcService", "Add", payload);
        assert(res.ok());

        calc::AddResponse resp;
        assert(resp.ParseFromString(res.response_payload));
        assert(resp.result() == i + j);
      }

      calc::AddRequest async_req;
      async_req.set_a(i);
      async_req.set_b(1000 + i);

      std::string async_payload;
      assert(async_req.SerializeToString(&async_payload));

      auto future = client.CallAsync("CalcService", "Add", async_payload);
      const auto async_res = future.get();
      assert(async_res.ok());

      calc::AddResponse async_resp;
      assert(async_resp.ParseFromString(async_res.response_payload));
      assert(async_resp.result() == i + 1000 + i);
    });
  }

  for (auto& t : clients) {
    t.join();
  }

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  std::cout << "server_multi_connection_test passed\n";
  return 0;
}
