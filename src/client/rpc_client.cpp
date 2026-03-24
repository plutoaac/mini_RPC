#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

#include "protocol/codec.h"

namespace rpc::client {

RpcClient::RpcClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port), next_id_(0) {}

RpcClient::~RpcClient() { Close(); }

bool RpcClient::Connect() {
  if (sock_) {
    return true;
  }

  common::UniqueFd conn_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!conn_fd) {
    std::cerr << "[ERROR] client socket failed: " << std::strerror(errno)
              << '\n';
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
    std::cerr << "[ERROR] invalid server address: " << host_ << '\n';
    Close();
    return false;
  }

  if (::connect(conn_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) < 0) {
    std::cerr << "[ERROR] connect failed: " << std::strerror(errno) << '\n';
    Close();
    return false;
  }

  sock_ = std::move(conn_fd);
  std::cout << "[INFO] connected to " << host_ << ':' << port_ << '\n';
  return true;
}

void RpcClient::Close() { sock_.Reset(); }

std::string RpcClient::NextRequestId() {
  const std::uint64_t id = ++next_id_;
  return std::to_string(id);
}

RpcCallResult RpcClient::Call(std::string_view service_name,
                              std::string_view method_name,
                              std::string_view request_payload) {
  if (!Connect()) {
    return RpcCallResult{{common::ErrorCode::kInternalError, "connect failed"},
                         {}};
  }

  rpc::RpcRequest request;
  request.set_request_id(NextRequestId());
  request.set_service_name(std::string(service_name));
  request.set_method_name(std::string(method_name));
  request.set_payload(std::string(request_payload));

  std::string write_error;
  if (!protocol::Codec::WriteMessage(sock_.Get(), request, &write_error)) {
    auto result = RpcCallResult{{common::ErrorCode::kInternalError,
                                 "send request failed: " + write_error},
                                {}};
    Close();
    return result;
  }

  rpc::RpcResponse response;
  std::string read_error;
  if (!protocol::Codec::ReadMessage(sock_.Get(), &response, &read_error)) {
    auto result = RpcCallResult{{common::ErrorCode::kInternalError,
                                 "read response failed: " + read_error},
                                {}};
    Close();
    return result;
  }

  if (response.request_id() != request.request_id()) {
    return RpcCallResult{
        {common::ErrorCode::kInternalError, "request_id mismatch"}, {}};
  }

  return RpcCallResult{
      {common::FromProtoErrorCode(response.error_code()), response.error_msg()},
      response.payload()};
}

}  // namespace rpc::client
