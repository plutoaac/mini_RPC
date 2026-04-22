#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "client/rpc_types.h"
#include "common/unique_fd.h"
#include "coroutine/task.h"

namespace rpc::client {

class PendingCalls;
class EventLoop;

/// RPC 客户端配置选项
/// 用于定制连接超时等行为。
struct RpcClientOptions {
  /// 发送超时时间（毫秒），默认 2000ms
  std::chrono::milliseconds send_timeout{2000};
  /// 接收超时时间（毫秒），默认 2000ms
  std::chrono::milliseconds recv_timeout{2000};
  /// 心跳发送间隔（秒），默认 30s。设为 0 则禁用心跳。
  std::chrono::seconds heartbeat_interval{30};
  /// 心跳超时关闭间隔（秒），默认 45s。设为 0 则禁用心跳。
  std::chrono::seconds heartbeat_timeout{45};
};

/// 阻塞式 RPC 客户端
///
/// 特点：
/// - 维护单个 TCP 连接，首次调用时自动建立连接
/// - 提供通用的 Call 接口，支持任意服务的任意方法调用
/// - 支持多线程并发 Call（写入串行化，响应由 dispatcher 线程分发）
///
/// 使用示例：
/// @code
/// rpc::client::RpcClient client("127.0.0.1", 8080);
/// if (client.Connect()) {
///   auto result = client.Call("Calculator", "Add", request_bytes);
///   if (result.ok()) {
///     // 处理 result.response_payload
///   }
/// }
/// @endcode
class RpcClient {
 public:
  /// 构造 RPC 客户端
  /// @param host 服务器地址（IP 或域名）
  /// @param port 服务器端口
  /// @param options 客户端配置选项（超时等）
  RpcClient(std::string host, std::uint16_t port,
            RpcClientOptions options = {});

  /// 析构函数，自动关闭连接
  ~RpcClient();

  /// 建立 TCP 连接
  /// 如果已连接则直接返回 true
  /// @return 连接成功返回 true，失败返回 false
  [[nodiscard]] bool Connect();

  /// 关闭当前连接
  /// 可以在后续调用中自动重连
  void Close();

  /// 通用 RPC 调用接口
  ///
  /// 执行一次 RPC 调用，自动处理连接、请求发送和响应接收。
  /// 如果连接断开会自动尝试重连。
  ///
  /// @param service_name 服务名称，如 "Calculator"
  /// @param method_name 方法名称，如 "Add"
  /// @param request_payload 业务请求 protobuf 序列化后的二进制数据
  /// @return RpcCallResult 包含调用状态和响应数据
  [[nodiscard]] std::future<RpcCallResult> CallAsync(
      std::string_view service_name, std::string_view method_name,
      std::string_view request_payload);

  /// 通用同步 RPC 调用接口
  ///
  /// 当前实现基于 CallAsync().get() 封装。
  ///
  /// @param service_name 服务名称，如 "Calculator"
  /// @param method_name 方法名称，如 "Add"
  /// @param request_payload 业务请求 protobuf 序列化后的二进制数据
  /// @return RpcCallResult 包含调用状态和响应数据
  [[nodiscard]] RpcCallResult Call(std::string_view service_name,
                                   std::string_view method_name,
                                   std::string_view request_payload);

  /// 通用协程 RPC 调用接口
  ///
  /// 直接将 coroutine waiter 绑定到 pending slot，由 dispatcher
  /// 按 request_id 完成并恢复协程。
  [[nodiscard]] rpc::coroutine::Task<RpcCallResult> CallCo(
      std::string_view service_name, std::string_view method_name,
      std::string_view request_payload);

 private:
  /// 生成下一个请求 ID
  /// 使用原子计数器保证唯一性
  /// @return 字符串形式的请求 ID
  [[nodiscard]] std::string NextRequestId();

  /// 发送心跳请求（空请求，service_name="__Heartbeat__"）
  /// 必须在 dispatcher 线程中调用（持有 write_mu_）
  void SendHeartbeatRequest();

  void DispatcherLoop();

  /// 服务器地址
  std::string host_;
  /// 服务器端口
  std::uint16_t port_;
  /// 客户端配置选项
  RpcClientOptions options_;
  /// socket 文件描述符，使用 RAII 包装自动管理生命周期
  rpc::common::UniqueFd sock_;
  std::mutex connect_mu_;
  std::mutex write_mu_;
  std::thread dispatcher_thread_;
  std::atomic<bool> dispatcher_running_{false};
  std::shared_ptr<EventLoop> event_loop_;
  /// 下一个请求 ID，原子变量保证线程安全的递增
  std::atomic<std::uint64_t> next_id_;
  std::shared_ptr<PendingCalls> pending_calls_;

  /// 最近一次从服务端收到任何数据的时间点（用于心跳检测）
  std::chrono::steady_clock::time_point last_activity_time_;
};

}  // namespace rpc::client
