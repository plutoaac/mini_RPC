#include "client/rpc_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <future>
#include <optional>
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

/// @brief 判断读取错误是否为临时/可恢复错误
/// @param read_error 错误描述字符串
/// @return 如果是临时错误返回 true，否则返回 false
///
/// 在非阻塞 I/O (MSG_DONTWAIT) 或设置了接收超时 (SO_RCVTIMEO) 的场景下，
/// recv() 失败不一定代表连接断开。
/// - "Resource temporarily unavailable": 对应
/// EAGAIN/EWOULDBLOCK，表示当前无数据可读，等待下一次 epoll 事件即可。
/// - "recv timeout": 对应 SO_RCVTIMEO
/// 超时，表示在指定时间内无数据，连接依然存活。
///
/// 其他错误（如 "Connection reset by peer"）则视为致命错误，通常需要关闭连接。
bool IsTemporaryReadFailure(std::string_view read_error) {
  return read_error.find("Resource temporarily unavailable") !=
             std::string_view::npos ||
         read_error.find("recv timeout") != std::string_view::npos;
}

constexpr std::size_t kFrameHeaderBytes = 4;
constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

/// 保留服务名：用于应用层心跳探测
inline constexpr std::string_view kHeartbeatServiceName = "__Heartbeat__";

/// 超过多久没收到数据就发送心跳请求
inline constexpr std::chrono::seconds kHeartbeatSendInterval{30};

/// 超过多久没收到数据就认为连接失活
inline constexpr std::chrono::seconds kHeartbeatTimeout{45};

bool HandleReadableFrames(std::string* read_buffer,
                          const std::shared_ptr<PendingCalls>& pending_calls,
                          std::chrono::steady_clock::time_point* last_activity,
                          int fd, std::mutex* write_mu,
                          const std::atomic<std::uint64_t>* next_id) {
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

    // 心跳响应特判：不走 pending_calls_ 完成路径。
    if (response.request_id().rfind("hb_", 0) == 0) {
      // 心跳 ACK，仅记录日志，不做任何业务处理。
      common::LogInfo("heartbeat ack received");
    } else {
      const RpcCallResult mapped_result{
          {common::FromProtoErrorCode(response.error_code()),
           response.error_msg()},
          response.payload()};
      if (!pending_calls->Complete(response.request_id(), mapped_result)) {
        common::LogWarn("response request_id not found in pending table: " +
                        response.request_id());
      }
    }

    offset += kFrameHeaderBytes + static_cast<std::size_t>(body_length);
  }

  if (offset > 0) {
    // 收到任何有效数据都刷新活跃时间。
    *last_activity = std::chrono::steady_clock::now();
    read_buffer->erase(0, offset);
  }
  return true;
}

/// @brief 协程式 RPC 调用的 Awaiter
///
/// DirectCallCoAwaiter 是 C++20 协程 Awaiter 模式的实现，用于支持
/// `co_await rpc_client.CallCo(...)` 语法。它将 RPC 调用的异步等待
/// 过程封装成协程可挂起/恢复的形式。
///
/// ## 工作流程
///
/// 1. CallCo() 发起请求后，创建 DirectCallCoAwaiter 并 co_await 它
/// 2. 编译器调用 await_ready()，决定是否需要挂起协程
/// 3. 如果需要挂起，编译器调用 await_suspend()，传入当前协程句柄
/// 4. await_suspend() 将句柄注册到 PendingCalls，等待响应
/// 5. Dispatcher 收到响应后，从 PendingCalls 取出句柄并 resume()
/// 6. 协程恢复，执行 await_resume()，获取 RPC 结果
///
/// ## 为什么需要这个 Awaiter？
///
/// 相比 CallAsync() 返回 std::future，CallCo() 返回 Task<RpcCallResult>，
/// 允许调用方用协程语法顺序编写异步代码：
///
/// ```cpp
/// // 使用 CallAsync（回调风格）
/// auto future = client.CallAsync("Service", "Method", request);
/// auto result = future.get();  // 阻塞等待
///
/// // 使用 CallCo（协程风格）
/// rpc::coroutine::Task<RpcCallResult> CoroCall() {
///   auto result = co_await client.CallCo("Service", "Method", request);
///   // 结果就绪，继续处理
///   co_return result;
/// }
/// ```
///
/// ## 关键设计：快路径优化
///
/// await_suspend() 返回 bool：
/// - 返回 true：挂起协程，等待响应到达后恢复
/// - 返回 false：不挂起协程，立即执行 await_resume()
///
/// 当响应已经先到达（BindCoroutine 返回 kAlreadyDone）时，
/// await_suspend() 返回 false，避免不必要的挂起/恢复开销。
class DirectCallCoAwaiter {
 public:
  /// @brief 构造函数
  /// @param pending_calls 请求管理器的共享指针
  /// @param request_id 本次请求的唯一标识符
  /// @param deadline 请求的超时时间点
  DirectCallCoAwaiter(std::shared_ptr<PendingCalls> pending_calls,
                      std::string request_id,
                      std::chrono::steady_clock::time_point deadline)
      : pending_calls_(std::move(pending_calls)),
        request_id_(std::move(request_id)),
        deadline_(deadline) {}

  /// @brief 检查是否需要挂起
  /// @return 始终返回 false，让 await_suspend 决定是否挂起
  ///
  /// @note 这里始终返回 false 是一种优化策略：
  ///       让 await_suspend 统一处理"需要挂起"和"已就绪"两种情况。
  ///       因为 BindCoroutine 已经有快路径逻辑，没必要在这里预检查。
  bool await_ready() const noexcept { return false; }

  /// @brief 挂起协程并注册等待
  /// @param handle 当前协程的句柄，用于后续恢复
  /// @return true 表示需要挂起，false 表示立即恢复
  ///
  /// ## 返回值含义
  ///
  /// | BindCoroutineStatus | 返回值 | 含义 |
  /// |---------------------|--------|------|
  /// | kBound              | true   | 注册成功，等待响应后恢复 |
  /// | kAlreadyDone        | false  | 响应已到，无需挂起（快路径）|
  /// | kNotFound           | false  | 请求不存在，返回错误 |
  /// | kAlreadyBound       | false  | 重复绑定，返回错误 |
  ///
  /// ## 快路径场景
  ///
  /// 当网络极快时，可能发生：协程刚执行到 await_suspend，响应已经到达。
  /// 此时 BindCoroutine 返回 kAlreadyDone，await_suspend 返回 false，
  /// 协程不会被挂起，直接进入 await_resume 获取结果。
  bool await_suspend(std::coroutine_handle<> handle) {
    // 将协程句柄注册到 PendingCalls，等待 Dispatcher 唤醒
    const auto bind_status =
        pending_calls_->BindCoroutine(request_id_, handle, deadline_);

    switch (bind_status) {
      case PendingCalls::BindCoroutineStatus::kBound:
        // 注册成功，返回 true 让协程挂起
        // 等 Dispatcher 收到响应后调用 resume() 恢复
        return true;

      case PendingCalls::BindCoroutineStatus::kAlreadyDone:
        // 快路径：响应已先到达，无需挂起
        // await_resume 会通过 TryPop 获取结果
        return false;

      case PendingCalls::BindCoroutineStatus::kNotFound:
        // 异常情况：请求槽位不存在（可能被其他线程清理）
        fallback_result_ = RpcCallResult{
            {common::make_error_code(common::ErrorCode::kInternalError),
             "bind coroutine waiter failed: request not found"},
            {}};
        return false;

      case PendingCalls::BindCoroutineStatus::kAlreadyBound:
        // 异常情况：槽位已被绑定（不应该发生，防御性编程）
        fallback_result_ = RpcCallResult{
            {common::make_error_code(common::ErrorCode::kInternalError),
             "bind coroutine waiter failed: already bound"},
            {}};
        return false;
    }

    // 兜底：未知状态
    fallback_result_ = RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "bind coroutine waiter failed: unexpected status"},
        {}};
    return false;
  }

  /// @brief 协程恢复时获取结果
  /// @return RPC 调用结果
  ///
  /// ## 调用时机
  ///
  /// 1. 正常路径：Dispatcher 收到响应 → Complete() → resume() → await_resume()
  /// 2. 快路径：await_suspend 返回 false → 立即执行 await_resume
  /// 3. 错误路径：await_suspend 设置了 fallback_result_ → 返回错误
  ///
  /// ## 结果获取方式
  ///
  /// - 有 fallback_result_：await_suspend 中发生了错误，返回该错误
  /// - TryPop 成功：响应已就绪，返回结果
  /// - TryPop 失败：异常情况（协程被错误地恢复），返回错误
  RpcCallResult await_resume() {
    // 优先检查 fallback_result_（错误路径）
    if (fallback_result_.has_value()) {
      return std::move(*fallback_result_);
    }

    // 正常路径：从 PendingCalls 取出结果
    // Complete() 已预先设置了 done=true 和 result
    if (auto popped = pending_calls_->TryPop(request_id_); popped.has_value()) {
      return std::move(*popped);
    }

    // 异常情况：协程被恢复但结果不存在
    // 可能原因：超时后被 FailTimedOut 清理，然后又被 resume
    return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "coroutine waiter resumed without result"},
        {}};
  }

 private:
  std::shared_ptr<PendingCalls> pending_calls_;     ///< 请求管理器
  std::string request_id_;                          ///< 请求 ID
  std::chrono::steady_clock::time_point deadline_;  ///< 超时时间点
  std::optional<RpcCallResult> fallback_result_;    ///< 错误情况下的兜底结果
};

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
    return false;
  }

  if (::connect(conn_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) < 0) {
    common::LogError(std::string("connect failed: ") + std::strerror(errno));
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

/// 发送心跳请求
/// 在 dispatcher 线程中调用（由 DispatcherLoop 的 timeout tick 触发）
void RpcClient::SendHeartbeatRequest() {
  rpc::RpcRequest request;
  request.set_request_id("hb_" + std::to_string(++next_id_));
  request.set_service_name(std::string(kHeartbeatServiceName));
  request.set_method_name("Ping");
  request.clear_payload();

  std::string write_error;
  {
    std::scoped_lock write_lock(write_mu_);
    if (!sock_ ||
        !protocol::Codec::WriteMessage(sock_.Get(), request, &write_error)) {
      common::LogError("heartbeat send failed: " +
                       (write_error.empty() ? std::string("connection closed")
                                            : write_error));
      return;
    }
  }
  common::LogInfo("heartbeat sent");
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

std::size_t RpcClient::GetInflightCount() const {
  if (!pending_calls_) {
    return 0;
  }
  return pending_calls_->Size();
}

bool RpcClient::IsConnected() const {
  std::scoped_lock lock(connect_mu_);
  return sock_.operator bool();
}

rpc::coroutine::Task<RpcCallResult> RpcClient::CallCo(
    std::string_view service_name, std::string_view method_name,
    std::string_view request_payload) {
  if (!Connect()) {
    co_return RpcCallResult{
        {common::make_error_code(common::ErrorCode::kInternalError),
         "connect failed"},
        {}};
  }

  const std::string request_id = NextRequestId();
  if (!pending_calls_->Add(request_id)) {
    co_return RpcCallResult{
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
      co_return popped.value_or(result);
    }
  }

  const auto deadline =
      std::chrono::steady_clock::now() + options_.recv_timeout;

  co_return co_await DirectCallCoAwaiter{pending_calls_, request_id, deadline};
}

/// RPC 响应分发循环
///
/// 在独立线程中运行，由 event loop 监听 socket 可读事件并分发响应。
///
/// 工作流程：
/// 1. 等待 socket 可读或 tick 超时
/// 2. 可读时读取并解析长度前缀帧
/// 3. 根据 response.request_id 完成 pending call
/// 4. tick 时触发按请求超时清理 + 心跳检测
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

  // 初始化活跃时间：连接建立时
  last_activity_time_ = std::chrono::steady_clock::now();
  auto last_heartbeat_time_ = last_activity_time_;

  // reactor 主循环：事件等待 + 读取解析 + request_id 分发 + 超时清理 +
  // 心跳检测。
  while (dispatcher_running_.load()) {
    switch (local_loop->WaitOnce(50)) {
      case EventLoop::WaitResult::kTimeout: {
        const auto now = std::chrono::steady_clock::now();

        // 先做业务请求的超时清理
        pending_calls_->FailTimedOut(
            now,
            RpcCallResult{
                {common::make_error_code(common::ErrorCode::kInternalError),
                 "call timeout"},
                {}});

        // 心跳检测（heartbeat_interval/timeout 为 0 则禁用）
        if (options_.heartbeat_timeout.count() > 0) {
          const auto idle = now - last_activity_time_;
          if (idle >= options_.heartbeat_timeout) {
            // 超过阈值没收到任何数据 → 认为连接失活
            common::LogError("connection inactive for " +
                             std::to_string(idle.count()) + "us, closing");
            pending_calls_->FailAll(RpcCallResult{
                {common::make_error_code(common::ErrorCode::kInternalError),
                 "connection inactive (heartbeat timeout)"},
                {}});
            dispatcher_running_.store(false);
            break;
          }
        }
        if (options_.heartbeat_interval.count() > 0) {
          if (now - last_activity_time_ >= options_.heartbeat_interval) {
            // Check if we already sent a heartbeat recently
            if (now - last_heartbeat_time_ >= options_.heartbeat_interval) {
              // 超过阈值没收到数据 → 发送心跳探测
              SendHeartbeatRequest();
              // 记录心跳发送时间，避免风暴
              last_heartbeat_time_ = now;
            }
          }
        }
        break;
      }
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
            // 收到任何原始数据都刷新活跃时间
            last_activity_time_ = std::chrono::steady_clock::now();
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

        if (!HandleReadableFrames(&read_buffer, pending_calls_,
                                  &last_activity_time_, fd, &write_mu_,
                                  &next_id_)) {
          dispatcher_running_.store(false);
          break;
        }

        if (peer_closed) {
          // 先解析已读缓存，再对仍未完成请求做失败收敛，避免"先到响应被覆盖"。
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
