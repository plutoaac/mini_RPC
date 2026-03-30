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

#include "coroutine/task.h"
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

rpc::RpcRequest MakeRequest(std::string request_id, std::string payload,
                            std::string method = "Ping") {
  rpc::RpcRequest request;
  request.set_request_id(std::move(request_id));
  request.set_service_name("EchoService");
  request.set_method_name(std::move(method));
  request.set_payload(std::move(payload));
  return request;
}

rpc::coroutine::Task<void> HandleConnectionMainCo(
    rpc::server::Connection* connection, bool* ok, std::string* error) {
  while (true) {
    if (connection->ShouldClose()) {
      *ok = true;
      co_return;
    }

    const bool read_ok = co_await connection->ReadRequestCo(error);
    if (!read_ok) {
      *ok = false;
      co_return;
    }

    if (connection->ShouldClose()) {
      *ok = true;
      co_return;
    }

    while (connection->HasPendingWrite()) {
      const bool write_ok = co_await connection->WriteResponseCo(error);
      if (!write_ok) {
        *ok = false;
        co_return;
      }

      if (connection->ShouldClose()) {
        *ok = true;
        co_return;
      }
    }
  }
}

void SetNonblock(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  assert(flags >= 0);
  assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

void TestCoroutineMainPathSuccess() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  rpc::server::Connection connection(std::move(server_fd), registry);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  rpc::RpcRequest req = MakeRequest("co-main-1", "hello-main");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  connection.NotifyReadable();
  assert(!task.IsReady());
  connection.NotifyWritable();

  rpc::RpcResponse resp;
  std::string read_error;
  assert(
      rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &read_error));
  assert(resp.request_id() == "co-main-1");
  assert(resp.error_code() == rpc::OK);
  assert(resp.payload() == "hello-main");

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  connection.NotifyReadable();
  task.Get();
  assert(ok);
}

void TestHalfPacketSuspendAndResume() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  rpc::server::Connection connection(std::move(server_fd), registry);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  rpc::RpcRequest req = MakeRequest("co-main-2", "partial");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(frame.size() > 6);

  assert(SendAll(client_fd.Get(), frame.data(), 3));
  connection.NotifyReadable();
  assert(!task.IsReady());

  assert(SendAll(client_fd.Get(), frame.data() + 3, frame.size() - 3));
  connection.NotifyReadable();
  connection.NotifyWritable();

  rpc::RpcResponse resp;
  std::string read_error;
  assert(
      rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &read_error));
  assert(resp.request_id() == "co-main-2");
  assert(resp.error_code() == rpc::OK);
  assert(resp.payload() == "partial");

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  connection.NotifyReadable();
  task.Get();
  assert(ok);
}

void TestWriteSuspendAndResume() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view) {
    return std::string(256 * 1024, 'w');
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  int send_buf = 1024;
  assert(::setsockopt(server_fd.Get(), SOL_SOCKET, SO_SNDBUF, &send_buf,
                      sizeof(send_buf)) == 0);

  rpc::server::Connection connection(std::move(server_fd), registry);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  rpc::RpcRequest req = MakeRequest("co-main-3", "x");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  connection.NotifyReadable();
  assert(!task.IsReady());

  auto reader = std::async(std::launch::async, [&]() {
    rpc::RpcResponse resp;
    std::string read_error;
    const bool read_ok =
        rpc::protocol::Codec::ReadMessage(client_fd.Get(), &resp, &read_error);
    assert(read_ok);
    assert(resp.request_id() == "co-main-3");
    assert(resp.error_code() == rpc::OK);
    assert(resp.payload().size() == 256 * 1024);
  });

  for (int i = 0; i < 20000 && !task.IsReady(); ++i) {
    connection.NotifyWritable();
    if ((i % 256) == 0) {
      std::this_thread::yield();
    }
  }

  reader.get();
  ::shutdown(client_fd.Get(), SHUT_RDWR);
  connection.NotifyReadable();
  task.Get();
  assert(ok);
}

void TestPeerCloseExit() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  rpc::server::Connection connection(std::move(server_fd), registry);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  ::shutdown(client_fd.Get(), SHUT_RDWR);
  client_fd.Reset();
  connection.NotifyReadable();
  task.Get();

  assert(ok);
  assert(connection.ShouldClose());
}

void TestReadTimeoutExit() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view request) {
    return std::string(request);
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  rpc::server::Connection::Options options;
  options.read_timeout = std::chrono::milliseconds(60);
  options.write_timeout = std::chrono::milliseconds(500);

  rpc::server::Connection connection(std::move(server_fd), registry, options);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  connection.Tick(std::chrono::steady_clock::now());
  task.Get();

  assert(ok);
  assert(connection.ShouldClose());
  assert(connection.GetState() == rpc::server::Connection::State::kError);
  assert(connection.LastError() == "read timeout");
}

void TestWriteTimeoutExit() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view) {
    return std::string(256 * 1024, 'z');
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  int send_buf = 1024;
  assert(::setsockopt(server_fd.Get(), SOL_SOCKET, SO_SNDBUF, &send_buf,
                      sizeof(send_buf)) == 0);

  rpc::server::Connection::Options options;
  options.read_timeout = std::chrono::milliseconds(500);
  options.write_timeout = std::chrono::milliseconds(60);

  rpc::server::Connection connection(std::move(server_fd), registry, options);
  bool ok = false;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  rpc::RpcRequest req = MakeRequest("co-main-4", "timeout-write");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  connection.NotifyReadable();
  assert(!task.IsReady());

  std::this_thread::sleep_for(std::chrono::milliseconds(80));
  connection.Tick(std::chrono::steady_clock::now());
  task.Get();

  assert(ok);
  assert(connection.ShouldClose());
  assert(connection.GetState() == rpc::server::Connection::State::kError);
  assert(connection.LastError() == "write timeout");
}

void TestBackpressureLimit() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("EchoService", "Ping", [](std::string_view) {
    return std::string(1024, 'b');
  }));

  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
  rpc::common::UniqueFd server_fd(fds[0]);
  rpc::common::UniqueFd client_fd(fds[1]);
  SetNonblock(server_fd.Get());

  rpc::server::Connection::Options options;
  options.max_write_buffer_bytes = 128;

  rpc::server::Connection connection(std::move(server_fd), registry, options);
  bool ok = true;
  std::string error;
  auto task = HandleConnectionMainCo(&connection, &ok, &error);

  rpc::RpcRequest req = MakeRequest("co-main-5", "backpressure");
  std::string frame;
  assert(BuildFrame(req, &frame));
  assert(SendAll(client_fd.Get(), frame.data(), frame.size()));

  connection.NotifyReadable();
  task.Get();

  assert(!ok);
  assert(connection.ShouldClose());
  assert(connection.GetState() == rpc::server::Connection::State::kError);
  assert(connection.LastError() == "write buffer backpressure limit exceeded");
}

}  // namespace

int main() {
  TestCoroutineMainPathSuccess();
  TestHalfPacketSuspendAndResume();
  TestWriteSuspendAndResume();
  TestPeerCloseExit();
  TestReadTimeoutExit();
  TestWriteTimeoutExit();
  TestBackpressureLimit();

  std::cout << "server_connection_coroutine_test passed\n";
  return 0;
}
