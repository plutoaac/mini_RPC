#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/rpc_error.h"
#include "common/unique_fd.h"

namespace rpc::client {

// RPC 调用结果：统一携带状态和响应 payload。
struct RpcCallResult {
  rpc::common::Status status{
      rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError),
      "uninitialized"};
  std::string response_payload;

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

// 阻塞式 RPC 客户端：维护单连接并提供通用 Call 接口。
class RpcClient {
 public:
  RpcClient(std::string host, std::uint16_t port);
  ~RpcClient();

  [[nodiscard]] bool Connect();
  void Close();

  // 通用 RPC 调用接口：
  // - request_payload：业务请求 protobuf 序列化后的 bytes。
  // - 返回值中 response_payload：业务响应 protobuf bytes。
  [[nodiscard]] RpcCallResult Call(std::string_view service_name,
                                   std::string_view method_name,
                                   std::string_view request_payload);

 private:
  [[nodiscard]] std::string NextRequestId();

  std::string host_;
  std::uint16_t port_;
  rpc::common::UniqueFd sock_;
  std::atomic<std::uint64_t> next_id_;
};

}  // namespace rpc::client
