#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/rpc_error.h"

namespace {

bool WaitServerReady(const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  rpc::client::RpcClient probe(
      "127.0.0.1", 50051,
      {.send_timeout = std::chrono::milliseconds(200),
       .recv_timeout = std::chrono::milliseconds(200)});

  while (std::chrono::steady_clock::now() - start < timeout) {
    if (probe.Connect()) {
      return true;
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

  const auto missing_res =
      client.Call("CalcService", "NoSuchMethod", add_payload);
  assert(!missing_res.ok());
  assert(missing_res.status.code ==
         rpc::common::make_error_code(rpc::common::ErrorCode::kMethodNotFound));

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  std::cout << "e2e_test passed\n";
  return 0;
}
