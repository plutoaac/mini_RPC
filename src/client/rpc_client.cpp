#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "client/pending_calls.h"
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
    : host_(std::move(host)),
      port_(port),
      options_(options),
      next_id_(0),
      pending_calls_(std::make_unique<PendingCalls>()) {}

RpcClient::~RpcClient() { Close(); }

bool RpcClient::Connect() {
  std::scoped_lock lock(connect_mu_);
  if (sock_) {
    return true;
  }

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
    return false;
  }

  sock_ = std::move(conn_fd);
  dispatcher_running_.store(true);
  dispatcher_thread_ = std::thread(&RpcClient::DispatcherLoop, this);

  common::LogInfo("connected to " + host_ + ':' + std::to_string(port_));
  return true;
}

void RpcClient::Close() {
  std::thread to_join;
  {
    std::scoped_lock lock(connect_mu_);
    dispatcher_running_.store(false);
    if (sock_) {
      // 主动半关闭可尽快唤醒阻塞在 recv 的 dispatcher 线程。
      (void)::shutdown(sock_.Get(), SHUT_RDWR);
    }
    sock_.Reset();
    if (dispatcher_thread_.joinable()) {
      to_join = std::move(dispatcher_thread_);
    }
  }

  if (to_join.joinable()) {
    to_join.join();
  }

  if (pending_calls_ != nullptr) {
    pending_calls_->FailAll(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connection closed"},
        {}});
  }
}

std::string RpcClient::NextRequestId() {
  const std::uint64_t id = ++next_id_;
  return std::to_string(id);
}

/// 通用 RPC 调用接口
///
/// 执行一次完整的 RPC 调用流程：建立连接 → 发送请求 → 等待响应 → 返回结果。
/// 使用 pending_calls_ 管理请求状态，为未来的异步调用模式预留扩展点。
///
/// @param service_name 服务名称（如 "Calculator"）
/// @param method_name 方法名称（如 "Add"）
/// @param request_payload 业务请求的 protobuf 序列化数据
/// @return RpcCallResult 包含调用状态和响应数据
RpcCallResult RpcClient::Call(std::string_view service_name,
                              std::string_view method_name,
                              std::string_view request_payload) {
  if (!Connect()) {
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connect failed"},
        {}};
  }

  const std::string request_id = NextRequestId();
  if (!pending_calls_->Add(request_id)) {
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "duplicate request_id in pending table"},
        {}};
  }

  rpc::RpcRequest request;
  request.set_request_id(request_id);
  request.set_service_name(std::string(service_name));
  request.set_method_name(std::string(method_name));
  request.set_payload(std::string(request_payload));

  std::string write_error;
  {
    std::scoped_lock write_lock(write_mu_);
    if (!sock_ ||
        !protocol::Codec::WriteMessage(sock_.Get(), request, &write_error)) {
      auto result = RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "send request failed: " + (write_error.empty()
                                          ? std::string("connection closed")
                                          : write_error)},
          {}};
      (void)pending_calls_->Complete(request_id, result);
      auto popped = pending_calls_->TryPop(request_id);
      Close();
      return popped.value_or(result);
    }
  }

  const auto completed =
      pending_calls_->WaitAndPop(request_id, options_.recv_timeout);
  if (!completed.has_value()) {
    pending_calls_->Remove(request_id);
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "call timeout"},
        {}};
  }

  return *completed;
}

void RpcClient::DispatcherLoop() {
  while (dispatcher_running_.load()) {
    int fd = -1;
    {
      std::scoped_lock lock(connect_mu_);
      if (!sock_) {
        break;
      }
      fd = sock_.Get();
    }

    rpc::RpcResponse response;
    std::string read_error;
    if (!protocol::Codec::ReadMessage(fd, &response, &read_error)) {
      if (!dispatcher_running_.load()) {
        break;
      }

      if (read_error.find("Resource temporarily unavailable") !=
              std::string::npos ||
          read_error.find("recv timeout") != std::string::npos) {
        continue;
      }

      if (read_error != "peer closed connection") {
        common::LogError("dispatcher read failed: " + read_error);
      }

      pending_calls_->FailAll(RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "dispatcher stopped: " + read_error},
          {}});
      dispatcher_running_.store(false);
      break;
    }

    const RpcCallResult mapped_result{
        {common::FromProtoErrorCode(response.error_code()),
         response.error_msg()},
        response.payload()};

    if (!pending_calls_->Complete(response.request_id(), mapped_result)) {
      common::LogWarn("response request_id not found in pending table: " +
                      response.request_id());
    }
  }
}

}  // namespace rpc::client
