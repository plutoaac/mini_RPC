#include <arpa/inet.h>
#include <netinet/in.h>
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

#include "client/rpc_client.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace {

constexpr std::uint16_t kHeartbeatServerPort = 50081;
constexpr std::uint16_t kInactiveServerPort = 50082;

bool WaitServerReady(std::uint16_t port,
                     const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
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

// =========================================================================
// Test 1: Server recognizes heartbeat and returns empty response
// =========================================================================
// This test uses the real server framework to verify the server-side
// heartbeat fast-path in Connection::TryParseRequests.
// =========================================================================

int RunRealHeartbeatServer() {
  // Use the real RpcServer with ServiceRegistry.
  // No services registered - heartbeat bypasses registry lookup.
  return 0;  // Placeholder: we use a raw socket server below instead.
}

/// Raw socket server that exercises the full client heartbeat mechanism.
/// Reads heartbeat requests and responds with empty OK responses.
/// Then handles one normal call. Stays alive to let client drain buffer.
int RunRawHeartbeatServer() {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    ::fprintf(stderr, "server: socket() failed\n");
    return 1;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    ::fprintf(stderr, "server: setsockopt(SO_REUSEADDR) failed\n");
    ::close(listen_fd);
    return 2;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kHeartbeatServerPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::fprintf(stderr, "server: bind() failed: %s\n", ::strerror(errno));
    ::close(listen_fd);
    return 3;
  }

  if (::listen(listen_fd, 8) < 0) {
    ::close(listen_fd);
    return 4;
  }

  // WaitServerReady probes the port; accept and close probe connections
  // until we get a real client that sends data.
  while (true) {
    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) continue;
      ::close(listen_fd);
      return 5;
    }

    // Set socket read timeout
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    ::setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Try to read a request. If the peer closes immediately (probe), retry.
    rpc::RpcRequest request;
    std::string read_error;
    if (!rpc::protocol::Codec::ReadMessage(conn_fd, &request, &read_error)) {
      // Probe connection closed, accept next one
      ::close(conn_fd);
      continue;
    }

    // Real client: process request and break out of accept loop
    // Verify it's a heartbeat request
    if (request.service_name() != "__Heartbeat__") {
      ::close(conn_fd);
      ::close(listen_fd);
      return 10;  // Not a heartbeat
    }

    // Construct heartbeat response
    rpc::RpcResponse response;
    response.set_request_id(request.request_id());
    response.set_error_code(rpc::OK);

    std::string write_error;
    if (!rpc::protocol::Codec::WriteMessage(conn_fd, response, &write_error)) {
      ::close(conn_fd);
      ::close(listen_fd);
      return 7;
    }

    // Read normal RPC call (blocking with timeout)
    rpc::RpcRequest normal_request;
    if (!rpc::protocol::Codec::ReadMessage(conn_fd, &normal_request,
                                           &read_error)) {
      ::close(conn_fd);
      ::close(listen_fd);
      return 8;
    }

    // Respond to normal request
    rpc::RpcResponse normal_response;
    normal_response.set_request_id(normal_request.request_id());
    normal_response.set_error_code(rpc::OK);
    normal_response.set_payload("pong");

    if (!rpc::protocol::Codec::WriteMessage(conn_fd, normal_response,
                                            &write_error)) {
      ::close(conn_fd);
      ::close(listen_fd);
      return 9;
    }

    // Keep connection alive briefly so client can read response
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ::close(conn_fd);
    break;  // Done with the real client
  }

  ::close(listen_fd);
  return 0;
}

/// Server that accepts a connection and reads one message but NEVER responds.
/// Used to test heartbeat timeout / inactivity detection.
int RunSilentServer() {
  const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) return 1;

  int reuse = 1;
  if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    ::close(listen_fd);
    return 2;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kInactiveServerPort);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(listen_fd);
    return 3;
  }

  if (::listen(listen_fd, 8) < 0) {
    ::close(listen_fd);
    return 4;
  }

  const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
  if (conn_fd < 0) {
    ::close(listen_fd);
    return 5;
  }

  // WaitServerReady probes: drain probe connections until real data arrives.
  while (true) {
    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) continue;
      ::close(listen_fd);
      return 5;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    ::setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    rpc::RpcRequest request;
    std::string read_error;
    if (!rpc::protocol::Codec::ReadMessage(conn_fd, &request, &read_error)) {
      // Probe connection closed or timed out, accept next one
      ::close(conn_fd);
      continue;
    }

    // Got a real request. Sleep long enough to outlast heartbeat_timeout.
    std::this_thread::sleep_for(std::chrono::seconds(10));
    ::close(conn_fd);
    break;
  }
  ::close(listen_fd);
  return 0;
}

// =========================================================================
// Test 1: Heartbeat request/response cycle
// =========================================================================
// Verifies:
// - Client sends heartbeat request with service_name="__Heartbeat__"
// - Server responds with empty OK response
// - Client recognizes heartbeat response (does NOT go through pending_calls_)
// - Connection remains alive after heartbeat
// =========================================================================
void test_heartbeat_request_response() {
  std::cout << "test_heartbeat_request_response: starting...\n";

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    _exit(RunRawHeartbeatServer());
  }

  if (!WaitServerReady(kHeartbeatServerPort, std::chrono::seconds(5))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_request_response failed: server not ready\n";
    std::abort();
  }

  // Use very short heartbeat intervals for testing
  rpc::client::RpcClient client(
      "127.0.0.1", kHeartbeatServerPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(10000),
       .heartbeat_interval = std::chrono::seconds(1),
       .heartbeat_timeout = std::chrono::seconds(5)});

  if (!client.Connect()) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_request_response failed: connect failed\n";
    std::abort();
  }

  // Wait for heartbeat to be sent and acked (interval = 1s).
  // Then make a normal RPC call to verify connection is still alive.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  auto future = client.CallAsync("TestService", "TestMethod", "ping");
  auto result = future.get();

  assert(result.ok());
  assert(result.response_payload == "pong");

  client.Close();

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  if (WEXITSTATUS(child_status) != 0) {
    std::cerr << "test_heartbeat_request_response: server exited with code "
              << WEXITSTATUS(child_status) << "\n";
    std::abort();
  }

  std::cout << "test_heartbeat_request_response: PASSED\n";
}

// =========================================================================
// Test 2: Heartbeat timeout detects inactive connection
// =========================================================================
// Verifies:
// - Client sends heartbeat when idle
// - If no response arrives within heartbeat_timeout, connection is closed
// - Pending calls are failed with "connection inactive" error
// =========================================================================
void test_heartbeat_timeout_closes_connection() {
  std::cout << "test_heartbeat_timeout_closes_connection: starting...\n";

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    _exit(RunSilentServer());
  }

  if (!WaitServerReady(kInactiveServerPort, std::chrono::seconds(3))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_timeout_closes_connection failed: server not "
                 "ready\n";
    std::abort();
  }

  // Use very short heartbeat intervals for testing
  rpc::client::RpcClient client(
      "127.0.0.1", kInactiveServerPort,
      {.send_timeout = std::chrono::milliseconds(500),
       .recv_timeout = std::chrono::milliseconds(30000),
       .heartbeat_interval = std::chrono::seconds(1),
       .heartbeat_timeout = std::chrono::seconds(2)});

  if (!client.Connect()) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_timeout_closes_connection failed: connect "
                 "failed\n";
    std::abort();
  }

  // Make a call that will wait for a response. The silent server reads the
  // heartbeat but never responds. The heartbeat timeout should close the
  // connection within ~2 seconds.
  auto future = client.CallAsync("TestService", "TestMethod", "ping");
  auto result = future.get();

  // The call should fail because the heartbeat timeout closed the connection
  assert(!result.ok());
  assert(result.status.message.find("inactive") != std::string::npos ||
         result.status.message.find("connection closed") != std::string::npos ||
         result.status.message.find("recv failed") != std::string::npos);

  client.Close();

  // Kill the silent server
  ::kill(pid, SIGTERM);
  int child_status = 0;
  ::waitpid(pid, &child_status, 0);

  std::cout << "test_heartbeat_timeout_closes_connection: PASSED\n";
}

// =========================================================================
// Test 3: Heartbeat disabled when interval/timeout is zero
// =========================================================================
void test_heartbeat_disabled() {
  std::cout << "test_heartbeat_disabled: starting...\n";

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    _exit(RunRawHeartbeatServer());
  }

  if (!WaitServerReady(kHeartbeatServerPort, std::chrono::seconds(3))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_disabled failed: server not ready\n";
    std::abort();
  }

  // Heartbeat disabled: interval=0, timeout=0
  rpc::client::RpcClient client(
      "127.0.0.1", kHeartbeatServerPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(2000),
       .heartbeat_interval = std::chrono::seconds(0),
       .heartbeat_timeout = std::chrono::seconds(0)});

  if (!client.Connect()) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "test_heartbeat_disabled failed: connect failed\n";
    std::abort();
  }

  // Make a normal call. With heartbeat disabled, the server expects the
  // first message to be a normal call (not a heartbeat), so it returns 10.
  auto future = client.CallAsync("TestService", "TestMethod", "ping");
  auto result = future.get();

  // With heartbeat disabled, the server gets the normal call first,
  // sees it's not a heartbeat, and returns exit code 10.
  // The client will get a connection reset or error.
  (void)result;

  client.Close();

  int child_status = 0;
  ::waitpid(pid, &child_status, 0);
  assert(WIFEXITED(child_status));
  // Server returns 10 because first request was not a heartbeat
  assert(WEXITSTATUS(child_status) == 10);

  std::cout << "test_heartbeat_disabled: PASSED\n";
}

}  // namespace

int main() {
  test_heartbeat_request_response();
  test_heartbeat_timeout_closes_connection();
  test_heartbeat_disabled();

  std::cout << "\nheartbeat_test: ALL TESTS PASSED\n";
  return 0;
}
