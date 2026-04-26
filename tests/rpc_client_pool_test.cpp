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
// 测试 1：RoundRobin 分发正确 + select_count 不重复计数
// ============================================================================

bool TestRoundRobin() {
  std::cout << "[TEST] RoundRobin distribution\n";

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

  // 发 9 次同步请求
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

  // 再发 6 次异步请求，验证 select_count 不翻倍
  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  for (int i = 0; i < 6; ++i) {
    futures.push_back(
        pool.CallAsync("CalcService", "Add", MakeAddPayload(1, 2)));
  }
  for (auto& f : futures) f.get();

  auto stats = pool.GetStats();
  std::size_t total_select = 0;
  for (const auto& s : stats) {
    total_select += s.select_count;
  }
  // 9 次 sync + 6 次 async = 15 次选择，select_count 应正好为 15
  if (total_select != 15) {
    std::cerr << "RoundRobin select_count mismatch: expected 15, got "
              << total_select << "\n";
    return false;
  }

  std::cout << "[PASS] RoundRobin distribution\n";
  return true;
}

// ============================================================================
// 测试 2：LeastInflight tie-break（所有节点 inflight 为 0 时不永远选第一个）
// ============================================================================

bool TestLeastInflightTieBreak() {
  std::cout << "[TEST] LeastInflight tie-break\n";

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
          .strategy = rpc::client::LoadBalanceStrategy::kLeastInflight,
          .max_consecutive_failures = 3});

  // 所有节点初始 inflight 都为 0，发多次请求验证不会永远选第一个
  for (int i = 0; i < 12; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    if (!res.ok()) {
      std::cerr << "LeastInflight tie-break call failed: "
                << res.status.message << "\n";
      return false;
    }
    int port_result = ParseAddResult(res.response_payload);
    port_counts[static_cast<std::uint16_t>(port_result)]++;
  }

  // 如果 tie-break 生效，每个端口应该都被选中若干次，不应出现某个端口为 0
  for (const auto& [port, count] : port_counts) {
    if (count == 0) {
      std::cerr << "LeastInflight tie-break failed: port " << port
                << " never selected (all zero tie not broken)\n";
      return false;
    }
  }

  std::cout << "[PASS] LeastInflight tie-break\n";
  return true;
}

// ============================================================================
// 测试 3：LeastInflight 在有真实 inflight 差异时能倾向轻载节点
// ============================================================================

bool TestLeastInflightPreference() {
  std::cout << "[TEST] LeastInflight preference under real inflight\n";

  // 50051: 快节点（无延迟）
  // 50052: 慢节点（300ms 延迟）
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
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
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

  // 用 Pool 自己的 CallAsync 向慢节点制造 inflight
  // 由于 LeastInflight 会优先选快节点，我们需要"强制"让一些请求落在慢节点。
  // 最简单的方式：先对 pool 发起 3 个 async 请求，此时由于初始 tie-break，
  // 请求会分散到两个节点。等慢节点的响应还在处理中时（inflight 仍存在），
  // 再发一批同步请求，观察是否倾向快节点。
  std::vector<std::future<rpc::client::RpcCallResult>> initial_futures;
  for (int i = 0; i < 3; ++i) {
    initial_futures.push_back(
        pool.CallAsync("CalcService", "Add", MakeAddPayload(i, 0)));
  }

  // 稍等让请求发出去，但不要等它们回来
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // 再发 6 次同步请求，观察选择倾向
  for (int i = 0; i < 6; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    if (!res.ok()) {
      std::cerr << "LeastInflight preference call failed: "
                << res.status.message << "\n";
      for (auto& f : initial_futures) f.get();
      return false;
    }
  }

  // 回收初始 futures
  for (auto& f : initial_futures) f.get();

  auto stats = pool.GetStats();
  int fast_hits = 0;
  int slow_hits = 0;
  for (const auto& s : stats) {
    if (s.endpoint == "127.0.0.1:50051") {
      fast_hits = static_cast<int>(s.select_count);
    } else if (s.endpoint == "127.0.0.1:50052") {
      slow_hits = static_cast<int>(s.select_count);
    }
  }

  // 总共 9 次选择（3 async + 6 sync）。
  // 期望快节点被选中次数明显多于慢节点（至少 >= 6）。
  if (fast_hits < 6) {
    std::cerr << "LeastInflight did not prefer fast node: fast=" << fast_hits
              << " slow=" << slow_hits << "\n";
    return false;
  }

  std::cout << "[PASS] LeastInflight preference (fast=" << fast_hits
            << " slow=" << slow_hits << ")\n";
  return true;
}

// ============================================================================
// 测试 4：Warmup 失败后节点被标记 unhealthy
// ============================================================================

bool TestWarmupMarksUnhealthy() {
  std::cout << "[TEST] Warmup failure marks unhealthy\n";

  auto server = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  // 50052 没有 server，Warmup 应该失败
  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(500),
                  .recv_timeout = std::chrono::milliseconds(500)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 2});

  bool ok = pool.Warmup();
  if (!ok) {
    std::cerr << "Warmup returned false unexpectedly\n";
    return false;
  }

  auto stats = pool.GetStats();
  bool found_healthy = false;
  bool found_unhealthy = false;
  for (const auto& s : stats) {
    if (s.endpoint == "127.0.0.1:50051" && s.healthy) {
      found_healthy = true;
    }
    if (s.endpoint == "127.0.0.1:50052" && !s.healthy) {
      found_unhealthy = true;
    }
  }

  if (!found_healthy) {
    std::cerr << "Warmup: expected 50051 to be healthy\n";
    return false;
  }
  if (!found_unhealthy) {
    std::cerr << "Warmup: expected 50052 to be unhealthy after warmup failure\n";
    return false;
  }

  std::cout << "[PASS] Warmup failure marks unhealthy\n";
  return true;
}

// ============================================================================
// 测试 5：某个 endpoint 挂掉时，其它 endpoint 仍可服务 + 重试不重复打同一节点
// ============================================================================

bool TestFailoverAndRetryExclusion() {
  std::cout << "[TEST] Failover and retry exclusion\n";

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

  assert(pool.Warmup());

  // 先发几次请求，确保两个 client 都已连接
  for (int i = 0; i < 4; ++i) {
    auto res = pool.Call("CalcService", "Add", MakeAddPayload(1, 2));
    assert(res.ok());
  }

  // 关掉 server1
  server1->Stop();
  server1.reset();

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 继续发请求，应该由 server2 处理
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

  std::cout << "[PASS] Failover and retry exclusion\n";
  return true;
}

// ============================================================================
// 测试 6：CallAsync / CallCo 在 pool 模式下仍正确工作 + select_count 不翻倍
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

  // 验证 select_count：6 async + 4 co = 10，不应翻倍
  auto stats = pool.GetStats();
  std::size_t total_select = 0;
  for (const auto& s : stats) {
    total_select += s.select_count;
  }
  if (total_select != 10) {
    std::cerr << "Async/Co select_count mismatch: expected 10, got "
              << total_select << "\n";
    return false;
  }

  std::cout << "[PASS] CallAsync and CallCo via pool\n";
  return true;
}

// ============================================================================
// 测试 7：乱序响应不会污染其它 endpoint 的结果匹配
// ============================================================================

bool TestOutOfOrderIsolation() {
  std::cout << "[TEST] Out-of-order response isolation\n";

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

  std::vector<std::future<rpc::client::RpcCallResult>> futures;
  std::vector<int> expected_results;
  for (int i = 0; i < 8; ++i) {
    expected_results.push_back(i + (i + 1));
    futures.push_back(
        pool.CallAsync("CalcService", "Add", MakeAddPayload(i, i + 1)));
  }

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
// 测试 8：CallCo 遇到连接错误后更新节点健康状态
// ============================================================================

bool TestCallCoHealthUpdate() {
  std::cout << "[TEST] CallCo health update on connection error\n";

  auto server = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  // 50052 没有 server
  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}, {"127.0.0.1", 50052}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(300),
                  .recv_timeout = std::chrono::milliseconds(300)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 1});

  // 连续发 2 次 CallCo，期望某次选到 50052 后标记 unhealthy
  for (int i = 0; i < 2; ++i) {
    auto task = pool.CallCo("CalcService", "Add", MakeAddPayload(1, 2));
    auto res = rpc::coroutine::SyncWait(std::move(task));
    // 只要有一次成功就行，不关心具体哪次
    (void)res;
  }

  auto stats = pool.GetStats();
  bool found_unhealthy = false;
  for (const auto& s : stats) {
    if (s.endpoint == "127.0.0.1:50052" && !s.healthy) {
      found_unhealthy = true;
    }
  }
  if (!found_unhealthy) {
    std::cerr << "CallCo: expected 50052 to be unhealthy after connection failure\n";
    return false;
  }

  std::cout << "[PASS] CallCo health update on connection error\n";
  return true;
}

// ============================================================================
// 测试 9：业务错误不会标记节点 unhealthy，但计入 fail_count
// ============================================================================

bool TestBusinessErrorNotUnhealthy() {
  std::cout << "[TEST] Business error does not affect health\n";

  auto server = std::make_unique<EmbeddedServer>(
      50051, [](std::string_view payload) -> std::string {
        calc::AddRequest req;
        (void)req.ParseFromString(std::string(payload));
        calc::AddResponse resp;
        resp.set_result(req.a() + req.b());
        std::string out;
        (void)resp.SerializeToString(&out);
        return out;
      });

  rpc::client::RpcClientPool pool(
      {{"127.0.0.1", 50051}},
      rpc::client::RpcClientPoolOptions{
          .client_options =
              rpc::client::RpcClientOptions{
                  .send_timeout = std::chrono::milliseconds(1000),
                  .recv_timeout = std::chrono::milliseconds(1000)},
          .strategy = rpc::client::LoadBalanceStrategy::kRoundRobin,
          .max_consecutive_failures = 1});

  assert(pool.Warmup());

  // 调用不存在的方法，应返回 METHOD_NOT_FOUND（业务错误）
  auto res = pool.Call("CalcService", "NoSuchMethod", MakeAddPayload(1, 2));
  if (res.ok()) {
    std::cerr << "Business error test: expected failure, got success\n";
    return false;
  }

  auto stats = pool.GetStats();
  const auto& s = stats.front();
  if (!s.healthy) {
    std::cerr << "Business error: node incorrectly marked unhealthy\n";
    return false;
  }
  if (s.fail_count != 1) {
    std::cerr << "Business error: fail_count expected 1, got " << s.fail_count
              << "\n";
    return false;
  }
  if (s.success_count != 0) {
    std::cerr << "Business error: success_count expected 0, got "
              << s.success_count << "\n";
    return false;
  }

  std::cout << "[PASS] Business error does not affect health\n";
  return true;
}

// ============================================================================
// Main
// ============================================================================

int main() {
  bool all_pass = true;
  all_pass &= TestRoundRobin();
  all_pass &= TestLeastInflightTieBreak();
  all_pass &= TestLeastInflightPreference();
  all_pass &= TestWarmupMarksUnhealthy();
  all_pass &= TestFailoverAndRetryExclusion();
  all_pass &= TestAsyncAndCoroutine();
  all_pass &= TestOutOfOrderIsolation();
  all_pass &= TestCallCoHealthUpdate();
  all_pass &= TestBusinessErrorNotUnhealthy();

  if (all_pass) {
    std::cout << "\n=== ALL TESTS PASSED ===\n";
    return 0;
  }
  std::cerr << "\n=== SOME TESTS FAILED ===\n";
  return 1;
}
