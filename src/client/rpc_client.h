#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace rpc::client {

class RpcClient {
 public:
  RpcClient(std::string host, std::uint16_t port);
  ~RpcClient();

  bool Connect();
  void Close();

  // Generic RPC call:
  // - request_payload: protobuf bytes of business request.
  // - response_payload: protobuf bytes of business response.
  // Returns true iff remote call succeeds with error_code == OK.
  bool Call(const std::string& service_name, const std::string& method_name,
            const std::string& request_payload, std::string* response_payload,
            int* error_code, std::string* error_msg);

 private:
  std::string NextRequestId();

  std::string host_;
  std::uint16_t port_;
  int sock_fd_;
  std::atomic<std::uint64_t> next_id_;
};

}  // namespace rpc::client
