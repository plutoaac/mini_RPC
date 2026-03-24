#pragma once

#include <string>

#include "common/rpc_error.h"

namespace rpc::client {

/// RPC 调用结果结构体
///
/// 封装了一次 RPC 调用的返回结果，包含状态信息和响应数据。
/// 这是对外暴露给调用方的核心数据结构。
///
/// 使用示例：
/// @code
/// RpcCallResult result = client.Call("Calculator", "Add", request_bytes);
/// if (result.ok()) {
///   // 成功，处理 result.response_payload
/// } else {
///   // 失败，查看 result.status
///   std::cout << result.status.message() << std::endl;
/// }
/// @endcode
struct RpcCallResult {
  /// 调用状态
  ///
  /// 包含错误码和错误信息。
  /// 默认初始化为内部错误状态，防止未初始化使用导致意外行为。
  /// - 如果调用成功：status.ok() == true
  /// - 如果调用失败：status.error_code() 和 status.message() 提供错误详情
  rpc::common::Status status{
      rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError),
      "uninitialized"};

  /// 响应负载
  ///
  /// 服务端返回的业务数据，是 protobuf 序列化后的二进制数据。
  /// 调用方需要根据业务 protobuf 定义进行反序列化。
  /// 仅当 status.ok() == true 时，此字段才有意义。
  std::string response_payload;

  /// 检查调用是否成功
  ///
  /// 这是一个便捷方法，等价于 status.ok()。
  /// @return 如果错误码为 OK 返回 true，否则返回 false
  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

}  // namespace rpc::client
