#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include "protocol/codec.h"
#include "rpc.pb.h"

namespace rpc::client {

RpcClient::RpcClient(std::string host, std::uint16_t port)
    : host_(std::move(host)), port_(port), sock_fd_(-1), next_id_(0) {}

RpcClient::~RpcClient() { Close(); }

bool RpcClient::Connect() {
  if (sock_fd_ >= 0) {
    return true;
  }

  sock_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd_ < 0) {
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

  if (::connect(sock_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    std::cerr << "[ERROR] connect failed: " << std::strerror(errno) << '\n';
    Close();
    return false;
  }

  std::cout << "[INFO] connected to " << host_ << ':' << port_ << '\n';
  return true;
}

void RpcClient::Close() {
  if (sock_fd_ >= 0) {
    ::close(sock_fd_);
    sock_fd_ = -1;
  }
}

std::string RpcClient::NextRequestId() {
  const std::uint64_t id = ++next_id_;
  return std::to_string(id);
}

bool RpcClient::Call(const std::string& service_name,
                     const std::string& method_name,
                     const std::string& request_payload,
                     std::string* response_payload, int* error_code,
                     std::string* error_msg) {
  if (response_payload == nullptr || error_code == nullptr ||
      error_msg == nullptr) {
    return false;
  }

  if (!Connect()) {
    *error_code = static_cast<int>(rpc::INTERNAL_ERROR);
    *error_msg = "connect failed";
    return false;
  }

  rpc::RpcRequest request;
  request.set_request_id(NextRequestId());
  request.set_service_name(service_name);
  request.set_method_name(method_name);
  request.set_payload(request_payload);

  std::string write_error;
  if (!protocol::Codec::WriteMessage(sock_fd_, request, &write_error)) {
    *error_code = static_cast<int>(rpc::INTERNAL_ERROR);
    *error_msg = "send request failed: " + write_error;
    Close();
    return false;
  }

  rpc::RpcResponse response;
  std::string read_error;
  if (!protocol::Codec::ReadMessage(sock_fd_, &response, &read_error)) {
    *error_code = static_cast<int>(rpc::INTERNAL_ERROR);
    *error_msg = "read response failed: " + read_error;
    Close();
    return false;
  }

  if (response.request_id() != request.request_id()) {
    *error_code = static_cast<int>(rpc::INTERNAL_ERROR);
    *error_msg = "request_id mismatch";
    return false;
  }

  *error_code = response.error_code();
  *error_msg = response.error_msg();
  *response_payload = response.payload();

  return response.error_code() == rpc::OK;
}

}  // namespace rpc::client
