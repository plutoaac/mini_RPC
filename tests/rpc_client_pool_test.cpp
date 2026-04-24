#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client_pool.h"
#include "common/rpc_error.h"
#include "coroutine/task.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

// ============================================================================
// 辅助函数
// ============================================================================

bool CanConnect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const bool ok =
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool WaitServerReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

std::string MakeAddPayload(int a, int b) {
  calc::AddRequest req;
  req.set_a(a);
  req.set_b(b);
  std::string payload;
  assert(req.SerializeToString(&payload));
  return payload;
}

int ParseAddResult(const std::string& payload) {
  calc::AddResponse resp;
  assert(resp.ParseFromString(payload));
  return resp.result();
}

// ============================================================================
// 内嵌 Server 封装
// ============================================================================

struct EmbeddedServer {
  rpc::server::ServiceRegistry registry;
  std::unique_ptr<rpc::server::RpcServer> server;
  std::thread thread;
  std::uint16_t port = 0;
  bool started = false;

  EmbeddedServer(std::uint16_t p,
                 std::function<std::string(std::string_view)> handler)
      : port(p) {
    assert(registry.Register("CalcService", "Add", std::move(handler)));
    server = std::make_unique<rpc::server::RpcServer>(port, registry, 2);
    thread = std::thread([this]() { started = server->Start(); });
    assert(WaitServerReady(port, std::chrono::seconds(3)));
  }

  void Stop() {
    if (server) {
      server->Stop();
    }
    if (thread.joinable()) {
      thread.join();
    }
  }

  ~EmbeddedServer() { Stop(); }
};

}  // namespace

// ============================================================================
// 测试 1：RoundRobin 分发正确
// ============================================================================

bool TestRoundRobin() {
  std::cout << "[TEST] RoundRobin distribution\n";

  // 3 个 server，每个返回自己的端口标识
  std::vector<std::unique_ptr<EmbeddedServer>> servers;
  std::unordered_map<std::uint16_t, int> port_counts;

  for (int i = 0; i < 3; ++i) {
    std::uint16_t port = static_cast<std::uint16_t>(50051 + i);
    port_counts[port] = 0;
    servers.push_back(std::make_unique<EmbeddedServer>(
        port, [port](std::string_view payload) -> std::string {
          calc::AddRequest req;
          (void)req.ParseFromString(std::string(payload));
          calc::AddResponse resp;
          resp.set_result(static_cast<int>(port));
          std::string out;
          (void)resp.SerializeToString(&out);
          return out;
        }));
  }

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}, {"127.0.0.1", 50053}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(1000),
                  .recv_timeout = std::chrono::milliseconds(1000)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 3});

  // 发 9 次请求，期望每个端口约 3 次
  for (int i = 0; i < 9; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    if (!res.ok()) {
      std::cerr << "RoundRobin call failed: " << res.status.message << "\n";
      return false;
    }
    int port_result = ParseAddResult(res.response_payload);
    port_counts[static_cast<std::uint16_t>(port_result)]++;
  }

  for (const auto& [port, count] : port_counts) {
    if (count != 3) {
      std::cerr << "RoundRobin uneven: port " << port << " got " << count
                << " (expected 3)\n";
      return false;
    }
  }

  // 验证 select_count 统计
  auto stats = pool.GetStats();
  for (const auto& s : stats) {
    if (s.select_count != 3) {
      std::cerr << "RoundRobin stats mismatch for " << s.endpoint << ": select="
                << s.select_count << "\n";
      return false;
    }
  }

  std::cout << "[PASS] RoundRobin distribution\n";
  return true;
}

// ============================================================================
// 测试 2：LeastInflight 倾向选择负载轻的连接
// ============================================================================

bool TestLeastInflight() {
  std::cout << "[TEST] LeastInflight preference\n";

  // 50051: 快节点（无延迟）
  // 50052: 慢节点（200ms 延迟）
  auto fast_server = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  auto slow_server = std::make_unique<EmbeddedServer>(
      50052, [](std::string_view payload) -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(2000),
                  .recv_timeout = std::chrono::milliseconds(2000)},
          .strategy = rpc::client::LoadBalanceStrategy::kLeastInflight,
          .max_consecutive_failures = 3});

  // 对慢节点制造 3 个 inflight（不发 future.get，让它们挂着）
  std::vector<std::future<rpc::client::RpcCallResult>> slow_futures;
  for (int i = 0; i < 3; ++i) {
    // 手动创建指向慢节点的 client 发请求，或者直接对 pool 里的底层 client 操作
    // 更简单：我们直接通过 Pool 发请求，但由于 LeastInflight 会优先选快节点，
    // 为了强制制造慢节点 inflight，我们直接对 50052 创建独立 client
    rpc::client::RpcClient slow_client("127.0.0.1", 50052);
    slow_futures.push_back(
        slow_client.CallAsync("CalcService", "Add", MakeAddPayload(i, 0)));
  }

  // 等待一小会儿，让 slow 节点的 inflight 确实建立起来
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 现在通过 Pool 发 10 次同步请求，期望绝大多数落在快节点
  int fast_hits = 0;
  int slow_hits = 0;
  for (int i = 0; i < 10; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    if (!res.ok()) {
      std::cerr << "LeastInflight call failed: " << res.status.message << "\n";
      // 释放 slow futures
      for (auto& f : slow_futures) f.get();
      return false;
    }
    // 判断是哪个节点：快节点在 50ms 内返回，慢节点要 200ms+
    // 但我们不知道具体哪个节点，只能靠 inflight 统计
  }

  // 通过 inflight 统计判断：发请求前快节点 inflight 应该更低
  auto stats = pool.GetStats();
  for (const auto& s : stats) {
    if (s.endpoint == "127.0.0.1:50051") {
      fast_hits = static_cast<int>(s.select_count);
    } else if (s.endpoint == "127.0.0.1:50052") {
      slow_hits = static_cast<int>(s.select_count);
    }
  }

  // 释放 slow futures
  for (auto& f : slow_futures) f.get();

  // 期望快节点被选中次数 >= 8（因为慢节点有 3 个 inflight）
  if (fast_hits < 8) {
    std::cerr << "LeastInflight did not prefer fast node: fast=" << fast_hits
              << " slow=" << slow_hits << "\n";
    return false;
  }

  std::cout << "[PASS] LeastInflight preference (fast=" << fast_hits
            << " slow=" << slow_hits << ")\n";
  return true;
}

// ============================================================================
// 测试 3：某个 endpoint 挂掉时，其它 endpoint 仍可服务
// ============================================================================

bool TestFailover() {
  std::cout << "[TEST] Failover when one endpoint down\n";

  auto server1 = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  auto server2 = std::make_unique<EmbeddedServer>(
      50052, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(500),
                  .recv_timeout = std::chrono::milliseconds(500)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 2});

  // 先预热并确保两个节点都 healthy
  assert(pool.Warmup());

  // 先发几次请求，确保两个 client 都已连接
  for (int i = 0; i < 4; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    assert(res.ok());
  }

  // 关掉 server1
  server1->Stop();
  server1.reset();

  // 给客户端一点时间检测到连接断开（或等下次调用时触发失败）
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 继续发请求，应该由 server2 处理（经过 1-2 次失败后 server1 被标记 unhealthy）
  for (int i = 0; i < 6; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    if (!res.ok()) {
      std::cerr << "Failover call failed (attempt " << i
                << "): " << res.status.message << "\n";
      return false;
    }
  }

  // 验证 server1 已被标记 unhealthy
  auto stats = pool.GetStats();
  bool found_unhealthy = false;
  for (const auto& s : stats) {
    if (s.endpoint == "127.0.0.1:50051" && !s.healthy) {
      found_unhealthy = true;
    }
  }
  if (!found_unhealthy) {
    std::cerr << "Failover: expected 50051 to be unhealthy\n";
    return false;
  }

  std::cout << "[PASS] Failover when one endpoint down\n";
  return true;
}

// ============================================================================
// 测试 4：CallAsync / CallCo 在 pool 模式下仍正确工作
// ============================================================================

bool TestAsyncAndCoroutine() {
  std::cout << "[TEST] CallAsync and CallCo via pool\n";

  auto server1 = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  auto server2 = std::make_unique<EmbeddedServer>(
      50052, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(1000),
                  .recv_timeout = std::chrono::milliseconds(1000)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 3});

  // CallAsync
  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  for (int i = 0; i < 6; ++i) {
    futures.push_back(
        pool.CallAsync("CalcService", "Add", MakeAddPayload(i, i + 1)));
  }
  for (int i = 0; i < 6; ++i) {
    auto res = futures[static_cast<std::size_t>(i)].get();
    if (!res.ok()) {
      std::cerr << "CallAsync failed: " << res.status.message << "\n";
      return false;
    }
    int result = ParseAddResult(res.response_payload);
    if (result != i + (i + 1)) {
      std::cerr << "CallAsync wrong result: expected " << (i + i + 1)
                << " got " << result << "\n";
      return false;
    }
  }

  // CallCo
  auto coro_task = [&pool]() -> rpc::coroutine::Task<bool> {
    for (int i = 0; i < 4; ++i) {
      auto res = co_await pool.CallCo("CalcService", "Add",
                                       MakeAddPayload(i, i * 2));
      if (!res.ok()) {
        std::cerr << "CallCo failed: " << res.status.message << "\n";
        co_return false;
      }
      int result = ParseAddResult(res.response_payload);
      if (result != i + i * 2) {
        std::cerr << "CallCo wrong result: expected " << (i + i * 2)
                  << " got " << result << "\n";
        co_return false;
      }
    }
    co_return true;
  };

  if (!rpc::coroutine::SyncWait(coro_task())) {
    return false;
  }

  std::cout << "[PASS] CallAsync and CallCo via pool\n";
  return true;
}

// ============================================================================
// 测试 5：乱序响应不会污染其它 endpoint 的结果匹配
// ============================================================================

bool TestOutOfOrderIsolation() {
  std::cout << "[TEST] Out-of-order response isolation\n";

  // server1: 50ms 延迟
  // server2: 10ms 延迟
  auto server1 = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  auto server2 = std::make_unique<EmbeddedServer>(
      50052, [](std::string_view payload) -> std::string {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(2000),
                  .recv_timeout = std::chrono::milliseconds(2000)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 3});

  // 先发多个 async 请求，它们会按 RoundRobin 分发到两个 server
  // 由于延迟不同，响应会乱序到达
  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  std::vector<int> expected_results;
  for (int i = 0; i < 8; ++i) {
    expected_results.push_back(i + (i + 1));
    futures.push_back(
        pool.CallAsync("CalcService", "Add", MakeAddPayload(i, i + 1)));
  }

  // 故意倒序 get，模拟乱序消费
  for (int i = 7; i >= 0; --i) {
    auto res = futures[static_cast<std::size_t>(i)].get();
    if (!res.ok()) {
      std::cerr << "OutOfOrder call failed: " << res.status.message << "\n";
      return false;
    }
    int result = ParseAddResult(res.response_payload);
    if (result != expected_results[static_cast<std::size_t>(i)]) {
      std::cerr << "OutOfOrder mismatch at index " << i << ": expected "
                << expected_results[static_cast<std::size_t>(i)] << " got "
                << result << "\n";
      return false;
    }
  }

  std::cout << "[PASS] Out-of-order response isolation\n";
  return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  bool all_pass = true;
  all_pass &= TestRoundRobin();
  all_pass &= TestLeastInflight();
  all_pass &= TestFailover();
  all_pass &= TestAsyncAndCoroutine();
  all_pass &= TestOutOfOrderIsolation();

  if (all_pass) {
    std::cout << "\n=== ALL TESTS PASSED ===\n";
    return 0;
  }
  std::cerr << "\n=== SOME TESTS FAILED ===\n";
  return 1;
}
