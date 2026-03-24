#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "rpc.pb.h"

namespace rpc::client {

struct RpcCallResult {
  rpc::ErrorCode error_code{rpc::INTERNAL_ERROR};
  std::string error_msg;
  std::string response_payload;

  [[nodiscard]] bool ok() const noexcept { return error_code == rpc::OK; }
};

class RpcClient {
 public:
  RpcClient(std::string host, std::uint16_t port);
  ~RpcClient();

  [[nodiscard]] bool Connect();
  void Close();

  // Generic RPC call (modern API):
  // - request_payload: protobuf bytes of business request.
  // - response_payload: protobuf bytes of business response in result.
  [[nodiscard]] RpcCallResult Call(std::string_view service_name,
                                   std::string_view method_name,
                                   std::string_view request_payload);

 private:
  [[nodiscard]] std::string NextRequestId();

  std::string host_;
  std::uint16_t port_;
  int sock_fd_;
  std::atomic<std::uint64_t> next_id_;
};

}  // namespace rpc::client
