#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/rpc_error.h"
#include "common/unique_fd.h"
#include "protocol/codec.h"
#include "rpc.pb.h"
#include "server/connection.h"
#include "server/service_registry.h"

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

rpc::RpcRequest MakeRequest(std::string request_id, std::string service,
                            std::string method, std::string payload) {
  rpc::RpcRequest request;
  request.set_request_id(std::move(request_id));
  request.set_service_name(std::move(service));
  request.set_method_name(std::move(method));
  request.set_payload(std::move(payload));
  return request;
}

}  // namespace

int main() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));
  assert(registry.Register("CrashService", "Boom", [](std::string_view) {
    throw std::runtime_error("boom");
    return std::string{};
  }));
  assert(registry.Register(
      "EchoService", "Large",
      [](std::string_view request) { return std::string(request); }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);

  std::thread server_thread([&registry, fd = std::move(server_fd)]() mutable {
    rpc::server::Connection connection(std::move(fd), registry);
    const bool ok = connection.Serve();
    assert(ok);
  });

  // 1) 单请求成功。
  {
    rpc::RpcRequest req = MakeRequest("req-1", "EchoService", "Ping", "hello");
    std::string frame;
    assert(BuildFrame(req, &frame));
    assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

    rpc::RpcResponse resp;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &error));
    assert(resp.request_id() == "req-1");
    assert(resp.error_code() == rpc::OK);
    assert(resp.payload() == "hello");
  }

  // 2) 半包输入仍可解析。
  {
    rpc::RpcRequest req =
        MakeRequest("req-2", "EchoService", "Ping", "partial");
    std::string frame;
    assert(BuildFrame(req, &frame));
    assert(frame.size() > 6);

    assert(SendAll(client_fd.Get(), frame.data(), 3));
    assert(SendAll(client_fd.Get(), frame.data() + 3, frame.size() - 3));

    rpc::RpcResponse resp;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &error));
    assert(resp.request_id() == "req-2");
    assert(resp.error_code() == rpc::OK);
    assert(resp.payload() == "partial");
  }

  // 3) 一次输入多个 frame 时全部处理。
  {
    rpc::RpcRequest req3 = MakeRequest("req-3", "EchoService", "Ping", "A");
    rpc::RpcRequest req4 = MakeRequest("req-4", "EchoService", "Ping", "B");

    std::string frame3;
    std::string frame4;
    assert(BuildFrame(req3, &frame3));
    assert(BuildFrame(req4, &frame4));

    std::string merged = frame3 + frame4;
    assert(SendAll(client_fd.Get(), merged.data(), merged.size()));

    rpc::RpcResponse resp3;
    rpc::RpcResponse resp4;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp3, &error));
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp4, &error));

    assert(resp3.request_id() == "req-3");
    assert(resp3.error_code() == rpc::OK);
    assert(resp3.payload() == "A");

    assert(resp4.request_id() == "req-4");
    assert(resp4.error_code() == rpc::OK);
    assert(resp4.payload() == "B");
  }

  // 4) 方法不存在返回 METHOD_NOT_FOUND。
  {
    rpc::RpcRequest req =
        MakeRequest("req-5", "EchoService", "NoSuchMethod", "x");
    std::string frame;
    assert(BuildFrame(req, &frame));
    assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

    rpc::RpcResponse resp;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &error));
    assert(resp.request_id() == "req-5");
    assert(resp.error_code() == rpc::METHOD_NOT_FOUND);
  }

  // 5) handler 异常返回 INTERNAL_ERROR。
  {
    rpc::RpcRequest req = MakeRequest("req-6", "CrashService", "Boom", "x");
    std::string frame;
    assert(BuildFrame(req, &frame));
    assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

    rpc::RpcResponse resp;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &error));
    assert(resp.request_id() == "req-6");
    assert(resp.error_code() == rpc::INTERNAL_ERROR);
  }

  // 额外覆盖：请求 protobuf 解析失败返回 PARSE_ERROR。
  {
    const std::uint32_t be_len = htonl(3U);
    std::string frame;
    frame.append(reinterpret_cast<const char*>(&be_len), 4);
    frame.append("BAD", 3);
    assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

    rpc::RpcResponse resp;
    std::string error;
    assert(rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &error));
    assert(resp.error_code() == rpc::PARSE_ERROR);
  }

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  client_fd.Reset();

  server_thread.join();

  // 6) 非阻塞写回下的部分写/回压场景：响应不丢失。
  {
    int pair_fds[2] = {-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair_fds) == 0);

    rpc::common::UniqueFd nb_server_fd(pair_fds[0]);
    rpc::common::UniqueFd nb_client_fd(pair_fds[1]);

    const int flags = ::fcntl(nb_server_fd.Get(), F_GETFL, 0);
    assert(flags >= 0);
    assert(::fcntl(nb_server_fd.Get(), F_SETFL, flags | O_NONBLOCK) == 0);

    int client_send_buf = 2 * 1024 * 1024;
    assert(::setsockopt(nb_client_fd.Get(), SOL_SOCKET, SO_SNDBUF,
                        &client_send_buf, sizeof(client_send_buf)) == 0);

    int send_buf = 1024;
    assert(::setsockopt(nb_server_fd.Get(), SOL_SOCKET, SO_SNDBUF, &send_buf,
                        sizeof(send_buf)) == 0);

    rpc::server::Connection nb_connection(std::move(nb_server_fd), registry);

    rpc::RpcRequest req = MakeRequest("req-7", "EchoService", "Large",
                                      std::string(512 * 1024, 'x'));
    std::string frame;
    assert(BuildFrame(req, &frame));

    auto sender = std::async(std::launch::async, [&]() {
      assert(SendAll(nb_client_fd.Get(), frame.data(), frame.size()));
    });

    std::string error;
    for (int i = 0; i < 1024 && !nb_connection.HasPendingWrite(); ++i) {
      assert(nb_connection.OnReadable(&error));
      if (!nb_connection.HasPendingWrite()) {
        std::this_thread::yield();
      }
    }
    assert(nb_connection.HasPendingWrite());
    sender.get();

    bool saw_backpressure = false;
    for (int i = 0; i < 32 && nb_connection.HasPendingWrite(); ++i) {
      assert(nb_connection.OnWritable(&error));
      if (nb_connection.HasPendingWrite()) {
        saw_backpressure = true;
      }
    }
    assert(saw_backpressure);

    auto reader = std::async(std::launch::async, [&]() {
      rpc::RpcResponse resp;
      std::string read_error;
      const bool ok = rpc::protocol::Codec::ReadMessage(nb_client_fd.Get(),
                                                        &resp, &read_error);
      assert(ok);
      assert(resp.request_id() == "req-7");
      assert(resp.error_code() == rpc::OK);
      assert(resp.payload().size() == 512 * 1024);
      assert(resp.payload()[0] == 'x');
      assert(resp.payload().back() == 'x');
    });

    for (int i = 0; i < 1024 && nb_connection.HasPendingWrite(); ++i) {
      assert(nb_connection.OnWritable(&error));
      if (nb_connection.ShouldClose()) {
        break;
      }
    }

    assert(!nb_connection.HasPendingWrite());
    reader.get();
  }

  std::cout << "server_connection_test passed\n";
  return 0;
}
