#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "protocol/codec.h"
#include "rpc.pb.h"
#include "server/service_registry.h"
#include "server/worker_loop.h"

namespace {

bool SendAll(int fd, const char* data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    const ssize_t rc = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (rc > 0) {
      sent += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

bool BuildFrame(const google::protobuf::Message& message, std::string* out) {
  if (out == nullptr) {
    return false;
  }

  const std::size_t body_len = static_cast<std::size_t>(message.ByteSizeLong());
  if (body_len == 0) {
    return false;
  }

  std::string body;
  body.resize(body_len);
  if (!message.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    return false;
  }

  const std::uint32_t be_len = htonl(static_cast<std::uint32_t>(body.size()));
  out->clear();
  out->reserve(4 + body.size());
  out->append(reinterpret_cast<const char*>(&be_len), 4);
  out->append(body);
  return true;
}

rpc::RpcRequest MakeRequest(std::string request_id, std::string payload) {
  rpc::RpcRequest request;
  request.set_request_id(std::move(request_id));
  request.set_service_name("EchoService");
  request.set_method_name("Ping");
  request.set_payload(std::move(payload));
  return request;
}

void SetNonblock(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

void WaitForNoConnection(rpc::server::WorkerLoop* worker) {
  for (int i = 0; i < 200 && worker->ConnectionCount() > 0U; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(worker->ConnectionCount() == 0U);
}

void WaitForConnectionCount(rpc::server::WorkerLoop* worker,
                            std::size_t expected) {
  for (int i = 0; i < 200 && worker->ConnectionCount() != expected; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(worker->ConnectionCount() == expected);
}

void TestWorkerLoopThreadedHandoffAndStop() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view req) {
    return std::string(req);
  }));

  rpc::server::WorkerLoop worker(0U, registry);
  std::string error;
  assert(worker.Init(&error));
  // worker 在独立线程运行，模拟真实服务端运行模式。
  assert(worker.Start(&error));
  assert(worker.IsAcceptingNewConnections());

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  // 通过队列跨线程投递连接，由 worker 线程完成接管。
  assert(worker.EnqueueConnection(std::move(server_fd), "threaded-handoff",
                                  &error));
  // 等价生命周期约束：先确认连接已纳入 worker 管理，再发送请求。
  WaitForConnectionCount(&worker, 1U);

  rpc::RpcRequest req = MakeRequest("mw-1", "hello-thread");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  rpc::RpcResponse resp;
  std::string read_error;
  assert(
      rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &read_error));
  assert(resp.request_id() == "mw-1");
  assert(resp.error_code() == rpc::OK);
  assert(resp.payload() == "hello-thread");

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  client_fd.Reset();

  WaitForNoConnection(&worker);
  assert(worker.TotalAcceptedCount() == 1U);

  worker.RequestStop();
  assert(!worker.IsAcceptingNewConnections());
  worker.Join();
}

void TestRoundRobinReadyStructure() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view req) {
    return std::string(req);
  }));

  rpc::server::WorkerLoop worker0(0U, registry);
  rpc::server::WorkerLoop worker1(1U, registry);
  std::string error;

  assert(worker0.Init(&error));
  assert(worker1.Init(&error));
  assert(worker0.Start(&error));
  assert(worker1.Start(&error));
  assert(worker0.IsAcceptingNewConnections());
  assert(worker1.IsAcceptingNewConnections());

  constexpr int kConnections = 6;
  std::vector<rpc::common::UniqueFd> client_fds;
  client_fds.reserve(kConnections);

  for (int i = 0; i < kConnections; ++i) {
    int fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    rpc::common::UniqueFd server_fd(fds[0]);
    SetNonblock(server_fd.Get());
    client_fds.emplace_back(fds[1]);

    // 按 round-robin 风格把连接分散到不同 worker。
    rpc::server::WorkerLoop* target = (i % 2 == 0) ? &worker0 : &worker1;
    assert(
        target->EnqueueConnection(std::move(server_fd), "rr-handoff", &error));
  }

  WaitForConnectionCount(&worker0, 3U);
  WaitForConnectionCount(&worker1, 3U);

  for (int i = 0; i < kConnections; ++i) {
    rpc::RpcRequest req =
        MakeRequest("rr-" + std::to_string(i), "p-" + std::to_string(i));
    std::string frame;
    assert(BuildFrame(req, &frame));
    assert(SendAll(client_fds[static_cast<std::size_t>(i)].Get(), frame.data(),
                   frame.size()));

    rpc::RpcResponse resp;
    std::string read_error;
    assert(rpc::protocol::Codec::ReadMessage(
        client_fds[static_cast<std::size_t>(i)].Get(), &resp, &read_error));
    assert(resp.error_code() == rpc::OK);
    assert(resp.payload() == "p-" + std::to_string(i));
  }

  for (auto& fd : client_fds) {
    ::shutdown(fd.Get(), SHUT_RDWR);
    fd.Reset();
  }

  WaitForNoConnection(&worker0);
  WaitForNoConnection(&worker1);

  assert(worker0.TotalAcceptedCount() == 3U);
  assert(worker1.TotalAcceptedCount() == 3U);

  worker0.RequestStop();
  worker1.RequestStop();
  assert(!worker0.IsAcceptingNewConnections());
  assert(!worker1.IsAcceptingNewConnections());
  worker0.Join();
  worker1.Join();
}

}  // namespace

int main() {
  TestWorkerLoopThreadedHandoffAndStop();
  TestRoundRobinReadyStructure();
  std::cout << "server_multi_worker_loop_test passed\n";
  return 0;
}
