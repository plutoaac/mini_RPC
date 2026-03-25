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

#include "client/event_loop.h"
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

bool IsTemporaryReadFailure(std::string_view read_error) {
  return read_error.find("Resource temporarily unavailable") !=
             std::string_view::npos ||
         read_error.find("recv timeout") != std::string_view::npos;
}

constexpr std::size_t kFrameHeaderBytes = 4;
constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

bool HandleReadableFrames(std::string* read_buffer,
                          const std::shared_ptr<PendingCalls>& pending_calls) {
  // 允许一次网络读取携带多帧响应：循环尽量解析完整帧，剩余半包留在缓冲区。
  std::size_t offset = 0;

  while (read_buffer->size() - offset >= kFrameHeaderBytes) {
    std::uint32_t be_length = 0;
    std::memcpy(&be_length, read_buffer->data() + offset, kFrameHeaderBytes);
    const std::uint32_t body_length = ntohl(be_length);
    if (body_length == 0 || body_length > kMaxFrameSize) {
      pending_calls->FailAll(RpcCallResult{
          {common::make_error_code(common::ErrorCode::kInternalError),
           "invalid response frame length"},
          {}});
      return false;
    }

    if (read_buffer->size() - offset <
        kFrameHeaderBytes + static_cast<std::size_t>(body_length)) {
      // 半包：等待后续可读事件补齐。
      break;
    }

    rpc::RpcResponse response;
    if (!response.ParseFromArray(
            read_buffer->data() + offset + kFrameHeaderBytes,
            static_cast<int>(body_length))) {
      pending_calls->FailAll(RpcCallResult{
          {common::make_error_code(common::ErrorCode::kParseError),
           "failed to parse RpcResponse"},
          {}});
      return false;
    }

    const RpcCallResult mapped_result{
        {common::FromProtoErrorCode(response.error_code()),
         response.error_msg()},
        response.payload()};
    if (!pending_calls->Complete(response.request_id(), mapped_result)) {
      common::LogWarn("response request_id not found in pending table: " +
                      response.request_id());
    }

    offset += kFrameHeaderBytes + static_cast<std::size_t>(body_length);
  }

  if (offset > 0) {
    read_buffer->erase(0, offset);
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

  if (!SetSocketTimeout(conn_fd.Get(), SO_RCVTIMEO, options_.recv_timeout,
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
    if (event_loop_ != nullptr) {
      event_loop_->Wakeup();
    }
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
/// 当前实现由 dispatcher 线程按 request_id 直接完成 future，无额外 watcher
/// 线程。
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

  const auto deadline =
      std::chrono::steady_clock::now() + options_.recv_timeout;
  if (!pending_calls_->BindAsync(request_id, std::move(promise), deadline)) {
    pending_calls_->Remove(request_id);
    return MakeReadyFuture(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "bind async waiter failed"},
        {}});
  }

  return future;
}

RpcCallResult RpcClient::Call(std::string_view service_name,
                              std::string_view method_name,
                              std::string_view request_payload) {
  return CallAsync(service_name, method_name, request_payload).get();
}

/// RPC 响应分发循环
///
/// 在独立线程中运行，由 event loop 监听 socket 可读事件并分发响应。
///
/// 工作流程：
/// 1. 等待 socket 可读或 tick 超时
/// 2. 可读时读取并解析长度前缀帧
/// 3. 根据 response.request_id 完成 pending call
/// 4. tick 时触发按请求超时清理
void RpcClient::DispatcherLoop() {
  int fd = -1;
  {
    std::scoped_lock lock(connect_mu_);
    if (!sock_) {
      return;
    }
    fd = sock_.Get();
  }

  auto local_loop = std::make_shared<EventLoop>();
  if (!local_loop->Init()) {
    pending_calls_->FailAll(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "event loop init failed"},
        {}});
    dispatcher_running_.store(false);
    return;
  }

  if (!local_loop->SetReadFd(fd)) {
    if (!dispatcher_running_.load()) {
      return;
    }
    pending_calls_->FailAll(RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "event loop add fd failed"},
        {}});
    dispatcher_running_.store(false);
    return;
  }

  {
    std::scoped_lock lock(connect_mu_);
    event_loop_ = local_loop;
  }

  std::string read_buffer;
  read_buffer.reserve(8192);

  // reactor 主循环：事件等待 + 读取解析 + request_id 分发 + 超时清理。
  while (dispatcher_running_.load()) {
    switch (local_loop->WaitOnce(50)) {
      case EventLoop::WaitResult::kTimeout:
        pending_calls_->FailTimedOut(
            std::chrono::steady_clock::now(),
            RpcCallResult{
                {common::make_error_code(common::ErrorCode::kInternalError),
                 "call timeout"},
                {}});
        break;
      case EventLoop::WaitResult::kWakeup:
        break;
      case EventLoop::WaitResult::kError:
        pending_calls_->FailAll(RpcCallResult{
            {common::make_error_code(common::ErrorCode::kInternalError),
             "dispatcher stopped: event loop error"},
            {}});
        dispatcher_running_.store(false);
        break;
      case EventLoop::WaitResult::kReadable: {
        bool keep_running = true;
        bool peer_closed = false;
        // 非阻塞 drain：一次可读事件尽可能把内核缓冲区读空，降低 event 次数。
        while (keep_running) {
          char chunk[4096];
          const ssize_t rc = ::recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
          if (rc > 0) {
            read_buffer.append(chunk, static_cast<std::size_t>(rc));
            continue;
          }
          if (rc == 0) {
            peer_closed = true;
            keep_running = false;
            break;
          }

          if (errno == EINTR) {
            continue;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }

          std::string read_error =
              std::string("recv failed: ") + std::strerror(errno);
          if (!IsTemporaryReadFailure(read_error)) {
            common::LogError("dispatcher read failed: " + read_error);
            pending_calls_->FailAll(RpcCallResult{
                {common::make_error_code(common::ErrorCode::kInternalError),
                 "dispatcher stopped: " + read_error},
                {}});
            dispatcher_running_.store(false);
            keep_running = false;
          }
          break;
        }

        if (!dispatcher_running_.load()) {
          break;
        }

        if (!HandleReadableFrames(&read_buffer, pending_calls_)) {
          dispatcher_running_.store(false);
          break;
        }

        if (peer_closed) {
          // 先解析已读缓存，再对仍未完成请求做失败收敛，避免“先到响应被覆盖”。
          if (pending_calls_->Size() > 0) {
            pending_calls_->FailAll(RpcCallResult{
                {common::make_error_code(common::ErrorCode::kInternalError),
                 "dispatcher stopped: peer closed connection"},
                {}});
          }
          dispatcher_running_.store(false);
        }
        break;
      }
    }
  }

  {
    std::scoped_lock lock(connect_mu_);
    event_loop_.reset();
  }
}

}  // namespace rpc::client
