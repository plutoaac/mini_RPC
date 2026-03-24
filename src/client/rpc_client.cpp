#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

#include "common/log.h"
#include "protocol/codec.h"

namespace rpc::client {

namespace {

bool SetSocketTimeout(int fd, int optname, std::chrono::milliseconds timeout,
                      std::string* error_msg) {
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto usec =
      std::chrono::duration_cast<std::chrono::microseconds>(timeout - sec);

  timeval tv{};
  tv.tv_sec = static_cast<decltype(tv.tv_sec)>(sec.count());
  tv.tv_usec = static_cast<decltype(tv.tv_usec)>(usec.count());

  if (::setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv)) < 0) {
    if (error_msg != nullptr) {
      *error_msg =
          std::string("setsockopt timeout failed: ") + std::strerror(errno);
    }
    return false;
  }

  return true;
}

}  // namespace

RpcClient::RpcClient(std::string host, std::uint16_t port,
                     RpcClientOptions options)
    : host_(std::move(host)), port_(port), options_(options), next_id_(0) {}

RpcClient::~RpcClient() { Close(); }

bool RpcClient::Connect() {
  if (sock_) {
    // 已连接直接复用。
    return true;
  }

  // 使用临时 RAII fd 承接连接过程，成功后再 move 到成员 sock_。
  common::UniqueFd conn_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!conn_fd) {
    common::LogError(std::string("client socket failed: ") +
                     std::strerror(errno));
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
    common::LogError("invalid server address: " + host_);
    Close();
    return false;
  }

  if (::connect(conn_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) < 0) {
    common::LogError(std::string("connect failed: ") + std::strerror(errno));
    Close();
    return false;
  }

  std::string timeout_error;
  if (!SetSocketTimeout(conn_fd.Get(), SO_SNDTIMEO, options_.send_timeout,
                        &timeout_error)) {
    common::LogError(timeout_error);
    Close();
    return false;
  }
  if (!SetSocketTimeout(conn_fd.Get(), SO_RCVTIMEO, options_.recv_timeout,
                        &timeout_error)) {
    common::LogError(timeout_error);
    Close();
    return false;
  }

  sock_ = std::move(conn_fd);
  common::LogInfo("connected to " + host_ + ':' + std::to_string(port_));
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
  // 先确保连接可用。
  if (!Connect()) {
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connect failed"},
        {}};
  }

  // 组装通用 RpcRequest，业务内容只作为 bytes 透传。
  rpc::RpcRequest request;
  request.set_request_id(NextRequestId());
  request.set_service_name(std::string(service_name));
  request.set_method_name(std::string(method_name));
  request.set_payload(std::string(request_payload));

  std::string write_error;
  // 发送请求帧。
  if (!protocol::Codec::WriteMessage(sock_.Get(), request, &write_error)) {
    auto result = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "send request failed: " + write_error},
        {}};
    Close();
    return result;
  }

  rpc::RpcResponse response;
  std::string read_error;
  // 读取响应帧。
  if (!protocol::Codec::ReadMessage(sock_.Get(), &response, &read_error)) {
    auto result = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "read response failed: " + read_error},
        {}};
    Close();
    return result;
  }

  if (response.request_id() != request.request_id()) {
    // 当前实现为串行请求，request_id 不匹配视为协议异常。
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "request_id mismatch"},
        {}};
  }

  return RpcCallResult{
      {common::FromProtoErrorCode(response.error_code()), response.error_msg()},
      response.payload()};
}

}  // namespace rpc::client
