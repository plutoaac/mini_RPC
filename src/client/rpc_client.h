#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/rpc_error.h"
#include "common/unique_fd.h"

namespace rpc::client {

struct RpcCallResult {
  rpc::common::Status status{rpc::common::ErrorCode::kInternalError,
                             "uninitialized"};
  std::string response_payload;

  [[nodiscard]] bool ok() const noexcept { return status.ok(); }
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
  rpc::common::UniqueFd sock_;
  std::atomic<std::uint64_t> next_id_;
};

}  // namespace rpc::client
