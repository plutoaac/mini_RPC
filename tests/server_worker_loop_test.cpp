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

void TestWorkerLoopSingleWorkerReadiness() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));

  rpc::server::WorkerLoop worker(0U, registry);
  std::string error;
  assert(worker.Init(&error));
  assert(error.empty());
  assert(worker.WorkerId() == 0U);
  assert(worker.ConnectionCount() == 0U);

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  assert(worker.AddConnection(std::move(server_fd), "local-test", &error));
  assert(worker.ConnectionCount() == 1U);

  rpc::RpcRequest req = MakeRequest("wk-1", "hello-worker");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  auto reader = std::async(std::launch::async, [&]() {
    rpc::RpcResponse resp;
    std::string read_error;
    const bool ok =
        rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &read_error);
    assert(ok);
    assert(resp.request_id() == "wk-1");
    assert(resp.error_code() == rpc::OK);
    assert(resp.payload() == "hello-worker");
  });

  for (int i = 0; i < 500 && reader.wait_for(std::chrono::milliseconds(0)) !=
                                 std::future_status::ready;
       ++i) {
    assert(worker.PollOnce(5, &error));
  }
  reader.get();

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  client_fd.Reset();

  for (int i = 0; i < 500 && worker.ConnectionCount() > 0U; ++i) {
    assert(worker.PollOnce(5, &error));
  }

  assert(worker.ConnectionCount() == 0U);
}

}  // namespace

int main() {
  TestWorkerLoopSingleWorkerReadiness();
  std::cout << "server_worker_loop_test passed\n";
  return 0;
}
