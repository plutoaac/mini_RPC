#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <string_view>
#include <thread>

#include "client/pending_calls.h"
#include "common/log.h"
#include "protocol/codec.h"

namespace rpc::client {

namespace {

std::future<RpcCallResult> MakeReadyFuture(RpcCallResult result) {
  std::promise<RpcCallResult> promise;
  auto future = promise.get_future();
  promise.set_value(std::move(result));
  return future;
}

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
      pending_calls_(std::make_shared<PendingCalls>()) {}

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

/// 通用 RPC 异步调用接口
///
/// 执行一次完整的 RPC 调用流程：建立连接 → 发送请求 → 异步等待响应。
/// 当前为最小桥接版：通过轻量等待线程将 pending table 的结果写入 future。
std::future<RpcCallResult> RpcClient::CallAsync(
    std::string_view service_name, std::string_view method_name,
    std::string_view request_payload) {
  if (!Connect()) {
    return MakeReadyFuture(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connect failed"},
        {}});
  }

  const std::string request_id = NextRequestId();
  if (!pending_calls_->Add(request_id)) {
    return MakeReadyFuture(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "duplicate request_id in pending table"},
        {}});
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
      return MakeReadyFuture(popped.value_or(result));
    }
  }

  std::promise<RpcCallResult> promise;
  auto future = promise.get_future();

  auto pending = pending_calls_;
  const auto timeout = options_.recv_timeout;
  std::thread([pending, request_id = std::move(request_id), timeout,
               promise = std::move(promise)]() mutable {
    const auto completed = pending->WaitAndPop(request_id, timeout);
    if (!completed.has_value()) {
      pending->Remove(request_id);
      promise.set_value(RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "call timeout"},
          {}});
      return;
    }

    promise.set_value(*completed);
  }).detach();

  return future;
}

RpcCallResult RpcClient::Call(std::string_view service_name,
                              std::string_view method_name,
                              std::string_view request_payload) {
  return CallAsync(service_name, method_name, request_payload).get();
}

/// RPC 响应分发循环
///
/// 在独立线程中运行，持续从服务器读取响应消息并分发给对应的等待调用。
/// 该循环是客户端实现同步调用的核心组件，将异步网络 IO 转换为同步等待模式。
///
/// 工作流程：
/// 1. 获取当前有效的 socket 文件描述符
/// 2. 阻塞读取服务器响应消息
/// 3. 根据响应中的 request_id 找到对应的等待请求并完成它
/// 4. 处理各种错误情况（超时、连接断开等）
void RpcClient::DispatcherLoop() {
  // 主循环：持续运行直到收到停止信号
  while (dispatcher_running_.load()) {
    // 获取 socket fd，需要加锁保证线程安全
    int fd = -1;
    {
      std::scoped_lock lock(connect_mu_);
      // 如果 socket 已关闭，退出循环
      if (!sock_) {
        break;
      }
      fd = sock_.Get();
    }

    // 尝试从服务器读取响应消息
    rpc::RpcResponse response;
    std::string read_error;
    if (!protocol::Codec::ReadMessage(fd, &response, &read_error)) {
      // 读取失败，检查是否是因为调度器已停止
      if (!dispatcher_running_.load()) {
        break;
      }

      // 如果是临时性错误（资源暂时不可用或接收超时），继续循环重试
      // 这些是非阻塞模式下的正常情况，不是真正的错误
      if (read_error.find("Resource temporarily unavailable") !=
              std::string::npos ||
          read_error.find("recv timeout") != std::string::npos) {
        continue;
      }

      // 记录错误日志（对端正常关闭连接的情况除外）
      if (read_error != "peer closed connection") {
        common::LogError("dispatcher read failed: " + read_error);
      }

      // 读取失败，将所有等待中的调用标记为失败
      pending_calls_->FailAll(RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "dispatcher stopped: " + read_error},
          {}});
      // 停止调度器
      dispatcher_running_.store(false);
      break;
    }

    // 读取成功，构造结果对象
    // 将 protobuf 错误码映射为本地错误码
    // mapped_result 随后会通过 Complete() 传递给对应的等待线程
    const RpcCallResult mapped_result{
        {common::FromProtoErrorCode(response.error_code()),
         response.error_msg()},
        response.payload()};

    // 根据 request_id 完成对应的等待调用
    // Complete() 会：
    // 1. 找到 request_id 对应的 Slot
    // 2. 将 mapped_result 存入 Slot
    // 3. 唤醒正在 WaitAndPop() 中等待的调用线程
    // 如果找不到对应的 request_id，说明该请求可能已超时被移除
    if (!pending_calls_->Complete(response.request_id(), mapped_result)) {
      common::LogWarn("response request_id not found in pending table: " +
                      response.request_id());
    }
  }
}

}  // namespace rpc::client
