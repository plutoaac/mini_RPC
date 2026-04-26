#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "client/load_balancer.h"
#include "client/rpc_client.h"
#include "client/rpc_types.h"
#include "coroutine/task.h"

namespace rpc::client {

/// 服务端端点描述
struct Endpoint {
  std::string host;
  std::uint16_t port;
};

/// 负载均衡策略枚举
enum class LoadBalanceStrategy {
  kRoundRobin,
  kLeastInflight,
};

/// 连接池配置选项
struct RpcClientPoolOptions {
  /// 底层单连接的通用配置
  RpcClientOptions client_options;
  /// 负载均衡策略
  LoadBalanceStrategy strategy = LoadBalanceStrategy::kRoundRobin;
  /// 连续失败多少次后标记节点为不健康
  std::size_t max_consecutive_failures = 3;
};

/// RPC 客户端连接池
///
/// 在现有单连接 RpcClient 之上的增量增强：
/// - 持有多个 endpoint，每个对应一个独立的 RpcClient（含独立连接、PendingCalls、dispatcher 线程）
/// - 支持 RoundRobin / LeastInflight 两种负载均衡策略
/// - 最小健康检查： unhealthy 节点会被剔除出候选集，直到恢复
/// - 同步 Call 路径支持失败重试（自动选择下一个健康节点）
/// - 对外保持与 RpcClient 一致的 Call / CallAsync / CallCo 接口
///
/// ## 线程安全
///
/// - SelectChannel / Call / CallAsync / CallCo 均可多线程并发调用
/// - 各 channel 的统计计数器使用原子变量
/// - 健康状态更新使用原子变量，无锁或最小锁
///
/// ## 设计取舍
///
/// 1. 不复用连接：每个 endpoint 独占一个 RpcClient，保持简单，不引入共享连接池的复杂状态机。
/// 2. 不重写 RpcClient：Pool 只做"选择 + 转发"，复用现有 Call/CallAsync/CallCo 链路。
/// 3. 异步/协程路径不重试：future/Task 已经发出后难以安全撤回，重试由同步 Call 承担。
/// 4. inflight 精确统计：直接透传 RpcClient::GetInflightCount()，避免 Pool 层自行维护的误差。
class RpcClientPool {
 public:
  /// 构造连接池
  /// @param endpoints 服务端地址列表（至少一个）
  /// @param options 连接池配置
  RpcClientPool(std::vector<Endpoint> endpoints,
                RpcClientPoolOptions options = {});

  ~RpcClientPool();

  // 禁止拷贝移动
  RpcClientPool(const RpcClientPool&) = delete;
  RpcClientPool& operator=(const RpcClientPool&) = delete;
  RpcClientPool(RpcClientPool&&) = delete;
  RpcClientPool& operator=(RpcClientPool&&) = delete;

  /// 预热：尝试与所有端点建立连接
  /// @return 是否至少成功连接一个端点
  bool Warmup();

  /// 同步 RPC 调用（支持失败重试）
  ///
  /// 内部会依次尝试健康节点，遇到连接类错误时自动尝试下一个节点，
  /// 直到成功或所有节点都尝试过。
  [[nodiscard]] RpcCallResult Call(std::string_view service_name,
                                   std::string_view method_name,
                                   std::string_view request_payload);

  /// 异步 RPC 调用（不重试）
  ///
  /// 选择一个健康节点后委托给该节点的 CallAsync。
  /// 若当前无健康节点，返回已完成的失败 future。
  ///
  /// @note 当前 CallAsync 只负责选择和转发，完成后的 health update 暂不做。
  ///       后续如果引入自定义 future/continuation，可补齐。
  [[nodiscard]] std::future<RpcCallResult> CallAsync(
      std::string_view service_name, std::string_view method_name,
      std::string_view request_payload);

  /// 协程 RPC 调用（不重试）
  ///
  /// 选择一个健康节点后委托给该节点的 CallCo。
  /// 完成后根据结果更新节点健康状态（仅连接类错误影响健康）。
  [[nodiscard]] rpc::coroutine::Task<RpcCallResult> CallCo(
      std::string_view service_name, std::string_view method_name,
      std::string_view request_payload);

  /// 每个端点的运行时统计快照
  struct EndpointStats {
    std::string endpoint;          // "host:port"
    std::size_t inflight;          // 当前未响应请求数
    std::size_t success_count;     // 累计成功次数
    std::size_t fail_count;        // 累计失败次数
    std::size_t select_count;      // 被负载均衡器选中次数
    bool healthy;                  // 当前健康状态
    bool connected;                // 当前是否已连接
  };

  /// 获取所有端点的统计快照
  [[nodiscard]] std::vector<EndpointStats> GetStats() const;

  /// 获取当前策略名称
  [[nodiscard]] const char* StrategyName() const;

 private:
  struct Channel;

  /// 选择一个可用 channel
  /// @param excluded 本轮已经尝试过的 channel，会被排除在候选之外
  /// @return 选中的 channel 指针；若无任何可用节点返回 nullptr
  Channel* SelectChannel(const std::vector<Channel*>& excluded);

  /// 简化版：选择可用 channel（无排除列表）
  Channel* SelectChannel();

  /// 记录节点可达性（只影响健康状态）
  /// @param channel 目标 channel
  /// @param reachable true 表示连接可达，false 表示连接/网络类失败
  void RecordReachability(Channel* channel, bool reachable);

  /// 记录调用结果（只影响 success/fail 统计）
  /// @param channel 目标 channel
  /// @param result 本次 RPC 调用结果
  void RecordCallResult(Channel* channel, const RpcCallResult& result);

  /// 判断错误是否为连接/网络类错误（决定是否标记节点不健康）
  static bool IsConnectionError(const RpcCallResult& result);

  std::vector<std::unique_ptr<Channel>> channels_;
  std::unique_ptr<LoadBalancer> balancer_;
  RpcClientPoolOptions options_;
};

}  // namespace rpc::client
