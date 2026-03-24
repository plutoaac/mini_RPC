#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>

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

void RpcClient::Close() {
  if (pending_calls_ != nullptr) {
    pending_calls_->FailAll(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connection closed"},
        {}});
  }
  sock_.Reset();
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
  // ========== 第一步：确保连接可用 ==========
  // Connect() 会检查现有连接，如果已连接则复用，否则新建连接
  if (!Connect()) {
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connect failed"},
        {}};
  }

  // ========== 第二步：生成请求 ID 并注册到 pending 表 ==========
  // request_id 用于匹配请求和响应，是异步 RPC 的关键标识
  const std::string request_id = NextRequestId();

  // 将 request_id 添加到 pending_calls_，为后续响应匹配做准备
  // 这一步在发送请求前完成，确保响应到达时能找到对应的槽位
  if (!pending_calls_->Add(request_id)) {
    // 添加失败，可能是 request_id 重复（理论上不应发生）
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "duplicate request_id in pending table"},
        {}};
  }

  // ========== 第三步：组装 RPC 请求消息 ==========
  // RpcRequest 是通用协议层消息，业务数据作为 payload 透传
  rpc::RpcRequest request;
  request.set_request_id(request_id);              // 请求唯一标识
  request.set_service_name(std::string(service_name));  // 目标服务
  request.set_method_name(std::string(method_name));    // 目标方法
  request.set_payload(std::string(request_payload));    // 业务数据（透传）

  // ========== 第四步：发送请求 ==========
  std::string write_error;
  if (!protocol::Codec::WriteMessage(sock_.Get(), request, &write_error)) {
    // 发送失败的处理流程：
    // 1. 构造失败结果
    // 2. 将失败结果填入 pending 表（Complete）
    // 3. 从 pending 表取出结果（Pop）
    // 4. 关闭连接（发送失败通常意味着连接有问题）
    auto result = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "send request failed: " + write_error},
        {}};
    (void)pending_calls_->Complete(request_id, result);  // 填充失败结果
    auto popped = pending_calls_->Pop(request_id);       // 取出并清理
    Close();
    return popped.value_or(result);  // 优先返回 popped，兜底返回 result
  }

  // ========== 第五步：读取响应 ==========
  // 阻塞等待服务端响应（受 recv_timeout 限制）
  rpc::RpcResponse response;
  std::string read_error;
  if (!protocol::Codec::ReadMessage(sock_.Get(), &response, &read_error)) {
    // 读取失败的处理流程与发送失败类似
    auto result = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "read response failed: " + read_error},
        {}};
    (void)pending_calls_->Complete(request_id, result);
    auto popped = pending_calls_->Pop(request_id);
    Close();
    return popped.value_or(result);
  }

  // ========== 第六步：处理响应 ==========
  // 将 protobuf 响应转换为 RpcCallResult
  const RpcCallResult mapped_result{
      {common::FromProtoErrorCode(response.error_code()), response.error_msg()},
      response.payload()};

  // 将响应结果填入 pending 表
  // 注意：这里使用 response.request_id() 而不是本地 request_id
  // 这是为了支持未来的"乱序响应"场景（响应顺序可能与请求顺序不同）
  if (!pending_calls_->Complete(response.request_id(), mapped_result)) {
    // Complete 失败，可能原因：
    // 1. 服务端返回了未发送过的 request_id
    // 2. 该请求已被 Pop 移除（并发场景）
    auto result = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "response request_id not found in pending table"},
        {}};
    (void)pending_calls_->Complete(request_id, result);
    auto popped = pending_calls_->Pop(request_id);
    return popped.value_or(result);
  }

  // ========== 第七步：取出并返回结果 ==========
  // 从 pending 表中取出已完成的结果
  const auto completed = pending_calls_->Pop(request_id);
  if (!completed.has_value()) {
    // 理论上不应发生，因为前面刚 Complete 成功
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "pending result missing for request_id=" + request_id},
        {}};
  }

  return *completed;
}

}  // namespace rpc::client
