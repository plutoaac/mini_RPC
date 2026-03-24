#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/rpc_error.h"
#include "common/unique_fd.h"

namespace rpc::client {

/// RPC 调用结果结构体
/// 统一携带调用状态和响应数据，便于调用方判断成功与否并获取结果。
struct RpcCallResult {
  /// 调用状态，包含错误码和错误信息
  /// 默认初始化为内部错误状态，防止未初始化使用
  rpc::common::Status status{
      rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError),
      "uninitialized"};

  /// 响应负载：业务响应 protobuf 序列化后的二进制数据
  std::string response_payload;

  /// 检查调用是否成功
  /// @return 状态码为 OK 时返回 true
  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

/// RPC 客户端配置选项
/// 用于定制连接超时等行为。
struct RpcClientOptions {
  /// 发送超时时间（毫秒），默认 2000ms
  std::chrono::milliseconds send_timeout{2000};
  /// 接收超时时间（毫秒），默认 2000ms
  std::chrono::milliseconds recv_timeout{2000};
};

/// 阻塞式 RPC 客户端
///
/// 特点：
/// - 维护单个 TCP 连接，首次调用时自动建立连接
/// - 提供通用的 Call 接口，支持任意服务的任意方法调用
/// - 线程不安全，如需多线程调用请使用多个客户端实例
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
  [[nodiscard]] RpcCallResult Call(std::string_view service_name,
                                   std::string_view method_name,
                                   std::string_view request_payload);

 private:
  /// 生成下一个请求 ID
  /// 使用原子计数器保证唯一性
  /// @return 字符串形式的请求 ID
  [[nodiscard]] std::string NextRequestId();

  /// 服务器地址
  std::string host_;
  /// 服务器端口
  std::uint16_t port_;
  /// 客户端配置选项
  RpcClientOptions options_;
  /// socket 文件描述符，使用 RAII 包装自动管理生命周期
  rpc::common::UniqueFd sock_;
  /// 下一个请求 ID，原子变量保证线程安全的递增
  std::atomic<std::uint64_t> next_id_;
};

}  // namespace rpc::client
