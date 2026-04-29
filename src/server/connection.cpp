/**
 * @file connection.cpp
 * @brief RPC 服务端连接处理模块实现
 *
 * 本文件实现了 Connection 类的所有方法，包括：
 * - Socket 数据读取与写入
 * - 帧协议的编码与解码
 * - RPC 请求的解析、处理与响应
 * - C++20 协程支持的异步 I/O 等待机制
 *
 * ## 实现要点
 *
 * 1. **非阻塞 I/O**
 *    - 所有 socket 操作都是非阻塞的
 *    - 正确处理 EAGAIN/EWOULDBLOCK 和 EINTR
 *
 * 2. **帧协议**
 *    - 4 字节大端序长度前缀
 *    - 最大帧大小限制为 4MB
 *
 * 3. **错误处理**
 *    - 解析错误返回错误响应，不关闭连接
 *    - 处理函数异常被捕获并转换为错误响应
 *
 * 4. **协程支持**
 *    - 实现 Awaiter 接口支持 co_await 异步等待
 *    - 与事件循环配合实现非阻塞异步 I/O
 *
 * @see connection.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/connection.h"

#include <arpa/inet.h>   // htonl, ntohl
#include <fcntl.h>       // fcntl, O_NONBLOCK
#include <sys/socket.h>  // recv, send, MSG_NOSIGNAL

#include <cerrno>  // errno, EINTR, EAGAIN, EWOULDBLOCK
#include <cstdlib>
#include <cstring>      // strerror
#include <exception>    // std::exception
#include <string>       // std::string
#include <string_view>  // std::string_view
#include <thread>
#include <utility>
#include <vector>  // std::vector

#include "common/log.h"               // 日志输出
#include "common/rpc_error.h"         // RPC 错误定义
#include "rpc.pb.h"                   // Protobuf 消息定义
#include "server/service_registry.h"  // 服务注册表

namespace rpc::server {

// ============================================================================
// 常量定义
// ============================================================================

namespace {

/// 帧头字节数（4 字节大端序长度前缀）
constexpr std::size_t kFrameHeaderBytes = 4;

/// 最大帧大小限制（4MB），防止恶意客户端发送超大帧
constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

/// 保留服务名：用于应用层心跳探测（与客户端约定）
inline constexpr std::string_view kHeartbeatServiceName = "__Heartbeat__";

}  // namespace

// ============================================================================
// 构造函数实现
// ============================================================================

/**
 * @brief Connection 构造函数
 *
 * 初始化连接对象，预分配缓冲区容量以提高性能。
 *
 * ## 初始化操作
 *
 * 1. 移动接管文件描述符
 * 2. 保存服务注册表引用
 * 3. 预分配缓冲区容量（减少后续重新分配）
 */
Connection::Connection(rpc::common::UniqueFd fd,
                       const ServiceRegistry& registry)
    : Connection(std::move(fd), registry, Options{}) {}

Connection::Connection(rpc::common::UniqueFd fd,
                       const ServiceRegistry& registry, Options options)
    : fd_(std::move(fd)),
      registry_(registry),
      options_(options),
      read_buffer_(8192) {
  // Buffer 构造函数已预分配 8192 字节
}

// ============================================================================
// 事件处理方法实现
// ============================================================================

/**
 * @brief 处理可读事件
 *
 * 读取 socket 数据并尝试解析处理请求。
 */
bool Connection::OnReadable(std::string* error_msg) {
  AssertOwnerThread();

  // 从 socket 读取数据
  const ReadResult read_result = ReadFromSocket(error_msg);

  // 处理不同的读取结果
  if (read_result == ReadResult::kPeerClosed) {
    // 对端关闭连接，标记为关闭收敛
    state_ = State::kClosing;
    should_close_ = true;
    return true;  // 正常情况，不是错误
  }
  if (read_result == ReadResult::kError) {
    // 发生读取错误
    EnterError(error_msg == nullptr ? "read error" : *error_msg);
    return false;
  }
  if (read_result == ReadResult::kWouldBlock) {
    // 没有数据可读（非阻塞模式下的正常情况）
    return true;
  }

  // 成功读取数据，尝试解析请求
  if (!TryParseRequests(error_msg)) {
    EnterError(error_msg == nullptr ? "parse/process error" : *error_msg);
    return false;
  }
  state_ = State::kOpen;
  return true;
}

/**
 * @brief 处理可写事件
 *
 * 发送写缓冲区中的数据。
 */
bool Connection::OnWritable(std::string* error_msg) {
  AssertOwnerThread();

  if (!FlushWrites(error_msg)) {
    EnterError(error_msg == nullptr ? "write error" : *error_msg);
    return false;
  }
  if (!HasPendingWrite()) {
    ClearWriteDeadline();
    state_ = State::kOpen;
  } else {
    ArmWriteDeadline();
  }
  return true;
}

// ============================================================================
// 状态查询方法实现
// ============================================================================

/**
 * @brief 检查是否有待发送的数据
 */
bool Connection::HasPendingWrite() const noexcept {
  return !write_buffer_.empty();
}

/**
 * @brief 检查连接是否应该关闭
 */
bool Connection::ShouldClose() const noexcept { return should_close_; }

Connection::State Connection::GetState() const noexcept { return state_; }

const char* Connection::StateName() const noexcept {
  switch (state_) {
    case State::kOpen:
      return "open";
    case State::kReading:
      return "reading";
    case State::kWriting:
      return "writing";
    case State::kClosing:
      return "closing";
    case State::kClosed:
      return "closed";
    case State::kError:
      return "error";
  }
  return "unknown";
}

const std::string& Connection::LastError() const noexcept {
  return last_error_;
}

/**
 * @brief 检查是否设置了请求分发器
 *
 * 用于判断连接是否配置了外部请求分发机制（如线程池）。
 * 当返回 true 时，请求会通过分发器异步处理；
 * 当返回 false 时，请求在当前协程中同步处理。
 *
 * @return true 已设置请求分发器
 * @return false 未设置请求分发器，使用默认同步处理
 */
bool Connection::HasRequestDispatcher() const noexcept {
  return static_cast<bool>(request_dispatcher_);
}

void Connection::BindToWorkerLoop(std::size_t worker_id) noexcept {
  owner_worker_id_ = worker_id;
  owner_thread_id_ = std::this_thread::get_id();
}

bool Connection::IsBoundToWorkerLoop() const noexcept {
  return owner_worker_id_.has_value();
}

std::optional<std::size_t> Connection::OwnerWorkerId() const noexcept {
  return owner_worker_id_;
}

bool Connection::IsOnOwnerThread() const noexcept {
  if (owner_thread_id_ == std::thread::id{}) {
    return true;
  }
  return owner_thread_id_ == std::this_thread::get_id();
}

/**
 * @brief 将连接标记为即将关闭
 *
 * 同时唤醒所有等待中的协程，使其能够正常退出。
 */
void Connection::MarkClosing() noexcept {
  AssertOwnerThread();

  if (state_ != State::kClosed && state_ != State::kError) {
    state_ = State::kClosing;
  }
  should_close_ = true;
  ClearReadDeadline();
  ClearWriteDeadline();
  // 唤醒等待读/写事件的协程，让它们能够检测到关闭状态并退出
  ResumeWaiter(read_waiter_);
  ResumeWaiter(write_waiter_);
}

void Connection::MarkClosed() noexcept {
  AssertOwnerThread();

  should_close_ = true;
  state_ = State::kClosed;
  read_ready_ = false;
  write_ready_ = false;
  ClearReadDeadline();
  ClearWriteDeadline();
  read_waiter_ = {};
  write_waiter_ = {};
}

void Connection::Tick(std::chrono::steady_clock::time_point now) noexcept {
  AssertOwnerThread();

  if (state_ == State::kClosed || state_ == State::kError || should_close_) {
    return;
  }

  if (read_deadline_.has_value() && now >= *read_deadline_) {
    EnterError("read timeout");
    return;
  }
  if (write_deadline_.has_value() && now >= *write_deadline_) {
    EnterError("write timeout");
  }
}

// ============================================================================
// 协程 Awaiter 实现
// ============================================================================
//
// ## 协程 Awaiter 设计概述
//
// 这些 Awaiter 类实现了 C++20 协程的 awaitable 接口，使得协程可以异步等待
// I/O 事件而不阻塞线程。这是实现高性能异步 RPC 服务端的关键组件。
//
// ### 工作原理
//
// ```
//   协程执行                    事件循环
//      |                          |
//      v                          |
//   co_await WaitReadableCo()     |
//      |                          |
//      v                          |
//   await_ready()? --> false      |
//      |                          |
//      v                          |
//   await_suspend(handle)         |
//      |                          |
//      | 保存 handle              |
//      | 到 read_waiter_          |
//      v                          |
//   协程挂起，控制权返回 --------> |
//                                 |
//                              epoll_wait() 等待事件
//                                 |
//                              socket 可读事件到达
//                                 |
//                              NotifyReadable() 被调用
//                                 |
//                                    v
//                              ResumeWaiter() 恢复协程
//                                 |
//   <-------------------------------
//      |
//      v
//   await_resume() 执行
//      |
//      v
//   协程继续执行
// ```
//
// ### Awaiter 的三个核心方法
//
// 1. **await_ready()**：检查是否需要挂起
//    - 返回 true：条件已满足，无需挂起，直接继续执行
//    - 返回 false：条件不满足，需要挂起协程
//
// 2. **await_suspend(handle)**：挂起协程
//    - 保存协程句柄，以便后续恢复
//    - 返回 true：真正挂起协程
//    - 返回 false：取消挂起，立即继续执行
//
// 3. **await_resume()**：协程恢复后执行
//    - 通常用于重置状态或返回值
//
// ### 线程模型
//
// - 协程挂起后，控制权返回给事件循环
// - 事件循环通过 epoll 等待 I/O 事件
// - I/O 事件到达时，事件循环调用 NotifyXxx() 恢复协程
// - 所有操作在单线程中完成，无需锁
//
// ============================================================================

/**
 * @brief 创建可读事件等待器
 *
 * 返回一个 Awaiter 对象，可在协程中 co_await 等待 socket 变为可读。
 *
 * ## 使用示例
 *
 * @code
 *   // 在协程中等待 socket 可读
 *   co_await conn->WaitReadableCo();
 *   // socket 可读或连接已关闭
 * @endcode
 *
 * @return ReadableAwaiter 可读事件等待器对象
 */
Connection::ReadableAwaiter Connection::WaitReadableCo() noexcept {
  return ReadableAwaiter(this);
}

/**
 * @brief 创建可写事件等待器
 *
 * 返回一个 Awaiter 对象，可在协程中 co_await 等待 socket 变为可写。
 *
 * ## 使用示例
 *
 * @code
 *   // 在协程中等待 socket 可写
 *   co_await conn->WaitWritableCo();
 *   // socket 可写或连接已关闭
 * @endcode
 *
 * @return WritableAwaiter 可写事件等待器对象
 */
Connection::WritableAwaiter Connection::WaitWritableCo() noexcept {
  return WritableAwaiter(this);
}

// ============================================================================
// 协程任务方法实现
// ============================================================================

/**
 * @brief 协程方式读取并处理请求
 *
 * 这是协程版本的请求读取方法。它会：
 * 1. 异步等待 socket 变为可读
 * 2. 调用 OnReadable() 读取数据并处理请求
 *
 * ## 设计要点
 *
 * - **非阻塞**：使用 co_await 异步等待，不会阻塞线程
 * - **取消支持**：如果连接被标记为关闭，立即返回 true
 * - **错误传播**：通过返回值指示处理是否成功
 *
 * ## 协程执行流程
 *
 * ```
 *   co_await WaitReadableCo()  -->  协程挂起，等待可读事件
 *            |
 *            v (事件到达，协程恢复)
 *   ShouldClose()?  -->  是  -->  co_return true
 *            |
 *            否
 *            v
 *   OnReadable()  -->  处理读取
 *            |
 *            v
 *   co_return result
 * ```
 *
 * @param error_msg 错误信息输出参数
 * @return Task<bool> 协程任务，返回处理是否成功
 */
rpc::coroutine::Task<bool> Connection::ReadRequestCo(std::string* error_msg) {
  // 等待 socket 可读，协程在此处可能挂起
  co_await WaitReadableCo();
  ClearReadDeadline();

  // 检查连接是否已被关闭，如果是则直接返回
  if (ShouldClose()) {
    co_return true;
  }

  // 执行实际的读取和处理操作
  co_return OnReadable(error_msg);
}

/**
 * @brief 协程方式发送响应数据
 *
 * 这是协程版本的响应发送方法。它会：
 * 1. 检查是否有待发送数据，没有则直接返回
 * 2. 异步等待 socket 变为可写
 * 3. 调用 OnWritable() 发送数据
 *
 * ## 设计要点
 *
 * - **快速路径优化**：如果没有待发送数据，立即返回，避免不必要的等待
 * - **非阻塞**：使用 co_await 异步等待，不会阻塞线程
 * - **取消支持**：如果连接被标记为关闭，立即返回 true
 *
 * ## 协程执行流程
 *
 * ```
 *   HasPendingWrite()?  -->  否  -->  co_return true (无数据需要发送)
 *            |
 *            是
 *            v
 *   co_await WaitWritableCo()  -->  协程挂起，等待可写事件
 *            |
 *            v (事件到达，协程恢复)
 *   ShouldClose()?  -->  是  -->  co_return true
 *            |
 *            否
 *            v
 *   OnWritable()  -->  发送数据
 *            |
 *            v
 *   co_return result
 * ```
 *
 * @param error_msg 错误信息输出参数
 * @return Task<bool> 协程任务，返回发送是否成功
 */
rpc::coroutine::Task<bool> Connection::WriteResponseCo(std::string* error_msg) {
  // 快速路径：如果没有待发送数据，直接返回成功
  if (!HasPendingWrite()) {
    ClearWriteDeadline();
    co_return true;
  }

  // 等待 socket 可写，协程在此处可能挂起
  co_await WaitWritableCo();
  ClearWriteDeadline();

  // 检查连接是否已被关闭，如果是则直接返回
  if (ShouldClose()) {
    co_return true;
  }

  // 执行实际的发送操作
  co_return OnWritable(error_msg);
}

// ============================================================================
// 事件通知方法实现
// ============================================================================

/**
 * @brief 通知连接可读
 *
 * 由事件循环在 socket 可读时调用，用于唤醒等待可读事件的协程。
 *
 * ## 调用时机
 *
 * - epoll 返回 EPOLLIN 事件时
 * - 收到数据需要处理时
 *
 * ## 实现逻辑
 *
 * 1. 设置 read_ready_ 标志为 true
 * 2. 如果有协程在等待，恢复其执行
 *
 * @note 此方法通常在事件循环线程中调用
 */
void Connection::NotifyReadable() noexcept {
  AssertOwnerThread();

  // 标记读就绪状态
  read_ready_ = true;
  state_ = State::kReading;
  // 唤醒等待可读事件的协程
  ResumeWaiter(read_waiter_);
}

/**
 * @brief 通知连接可写
 *
 * 由事件循环在 socket 可写时调用，用于唤醒等待可写事件的协程。
 *
 * ## 调用时机
 *
 * - epoll 返回 EPOLLOUT 事件时
 * - socket 发送缓冲区有空间时
 *
 * ## 实现逻辑
 *
 * 1. 设置 write_ready_ 标志为 true
 * 2. 如果有协程在等待，恢复其执行
 *
 * @note 此方法通常在事件循环线程中调用
 */
void Connection::NotifyWritable() noexcept {
  AssertOwnerThread();

  // 标记写就绪状态
  write_ready_ = true;
  state_ = State::kWriting;
  // 唤醒等待可写事件的协程
  ResumeWaiter(write_waiter_);
}

/**
 * @brief 设置请求分发器
 *
 * 配置外部请求分发机制，允许将请求处理委托给线程池等异步处理框架。
 * 当设置了分发器后，TryParseRequests() 会通过分发器异步处理请求，
 * 而不是在当前协程中同步调用 HandleOneRequest()。
 *
 * ## 使用场景
 *
 * - 配合 WorkerLoop + ThreadPool 实现请求的异步处理
 * - 将耗时的业务逻辑从 I/O 线程分离，提高并发性能
 *
 * ## 线程安全
 *
 * 必须在 owner 线程（worker loop 线程）中调用。
 *
 * @param dispatcher 请求分发函数，接收 DispatchRequest 并返回是否分发成功
 */
void Connection::SetRequestDispatcher(RequestDispatchFn dispatcher) {
  AssertOwnerThread();
  request_dispatcher_ = std::move(dispatcher);
}

/**
 * @brief 将响应加入发送队列
 *
 * 将响应序列化并加入写缓冲区，等待后续发送。
 * 与 QueueResponse() 的区别在于此方法会设置写超时定时器。
 *
 * ## 设计要点
 *
 * - 线程安全：必须在 owner 线程调用
 * - 超时管理：入队成功后启动写超时定时器，防止响应长时间未发送
 *
 * @param response 要发送的 RPC 响应
 * @param error_msg 错误信息输出参数
 * @return true 入队成功
 * @return false 入队失败（如缓冲区满、序列化失败等）
 */
bool Connection::EnqueueResponse(const rpc::RpcResponse& response,
                                 std::string* error_msg) {
  AssertOwnerThread();
  const bool queued = QueueResponse(response, error_msg);
  if (queued) {
    ArmWriteDeadline();
  }
  return queued;
}

/**
 * @brief 恢复等待中的协程
 *
 * 安全地恢复一个挂起的协程。如果协程句柄为空，则不做任何操作。
 *
 * ## 实现要点
 *
 * 1. 检查句柄是否有效
 * 2. 先清空 waiter_ 再恢复协程（防止重入问题）
 * 3. 调用 handle.resume() 恢复协程执行
 *
 * ## 重入安全
 *
 * 使用临时变量保存句柄并立即清空 waiter_，这样可以防止：
 * - 协程恢复后立即再次等待导致的问题
 * - 多次恢复同一个协程
 *
 * @param waiter 协程句柄的引用，恢复后会被清空
 */
void Connection::ResumeWaiter(std::coroutine_handle<>& waiter) noexcept {
  // 检查是否有等待中的协程
  if (!waiter) {
    return;
  }
  // 保存句柄并清空 waiter_，防止重入
  const auto handle = waiter;
  waiter = {};
  // 恢复协程执行
  handle.resume();
}

void Connection::AssertOwnerThread() const noexcept {
#ifndef NDEBUG
  if (!IsOnOwnerThread()) {
    common::LogError(
        "connection accessed from non-owner thread (worker-owned object)");
    std::abort();
  }
#endif
}

void Connection::EnterError(std::string message) noexcept {
  if (!message.empty()) {
    last_error_ = std::move(message);
  }
  should_close_ = true;
  state_ = State::kError;
  ClearReadDeadline();
  ClearWriteDeadline();
  ResumeWaiter(read_waiter_);
  ResumeWaiter(write_waiter_);
}

void Connection::ArmReadDeadline() noexcept {
  read_deadline_ = std::chrono::steady_clock::now() + options_.read_timeout;
}

void Connection::ArmWriteDeadline() noexcept {
  write_deadline_ = std::chrono::steady_clock::now() + options_.write_timeout;
}

void Connection::ClearReadDeadline() noexcept { read_deadline_.reset(); }

void Connection::ClearWriteDeadline() noexcept { write_deadline_.reset(); }

// ============================================================================
// ReadableAwaiter 实现
// ============================================================================

/**
 * @brief 检查是否需要等待可读事件
 *
 * 此方法在协程执行 co_await 时首先被调用。
 *
 * ## 返回值语义
 *
 * - **返回 true**：条件已满足，无需挂起协程
 *   - 连接已标记为关闭（should_close_ 为 true）
 *   - socket 已经可读（read_ready_ 为 true）
 * - **返回 false**：需要挂起协程等待可读事件
 *
 * @return true 不需要挂起，直接继续执行
 * @return false 需要挂起协程
 */
bool Connection::ReadableAwaiter::await_ready() const noexcept {
  // 如果连接已关闭或已可读，则无需等待
  return connection_->should_close_ || connection_->read_ready_;
}

/**
 * @brief 挂起协程以等待可读事件
 *
 * 当 await_ready() 返回 false 时调用此方法。
 *
 * ## 挂起流程
 *
 * 1. 再次检查条件（防止竞态条件）
 * 2. 如果条件已满足，返回 false 取消挂起
 * 3. 否则保存协程句柄到 read_waiter_，返回 true 挂起协程
 *
 * ## 双重检查的原因
 *
 * 在多线程或事件驱动的环境中，状态可能在 await_ready() 和 await_suspend()
 * 之间发生变化。例如：
 * - 连接可能在检查后被关闭
 * - 数据可能在挂起前到达
 *
 * 因此需要再次检查，避免不必要的挂起或错过事件。
 *
 * @param handle 要挂起的协程句柄
 * @return true 成功挂起协程，等待后续恢复
 * @return false 取消挂起，协程立即继续执行
 */
bool Connection::ReadableAwaiter::await_suspend(
    std::coroutine_handle<> handle) noexcept {
  // 再次检查条件，防止竞态条件
  if (connection_->should_close_ || connection_->read_ready_) {
    return false;  // 取消挂起
  }
  // 保存协程句柄，等待 NotifyReadable() 恢复
  connection_->read_waiter_ = handle;
  connection_->state_ = State::kReading;
  connection_->ArmReadDeadline();
  return true;  // 真正挂起协程
}

/**
 * @brief 协程恢复后执行的操作
 *
 * 当协程从挂起状态恢复时调用，用于清理状态。
 *
 * ## 实现逻辑
 *
 * 重置 read_ready_ 标志，表示已处理了当前的可读事件。
 * 这确保下一次 co_await 会正确等待新的事件。
 */
void Connection::ReadableAwaiter::await_resume() noexcept {
  // 清除读就绪标志
  connection_->read_ready_ = false;
}

// ============================================================================
// WritableAwaiter 实现
// ============================================================================

/**
 * @brief 检查是否需要等待可写事件
 *
 * 此方法在协程执行 co_await 时首先被调用。
 *
 * ## 返回值语义
 *
 * - **返回 true**：条件已满足，无需挂起协程
 *   - 连接已标记为关闭（should_close_ 为 true）
 *   - socket 已经可写（write_ready_ 为 true）
 * - **返回 false**：需要挂起协程等待可写事件
 *
 * @return true 不需要挂起，直接继续执行
 * @return false 需要挂起协程
 */
bool Connection::WritableAwaiter::await_ready() const noexcept {
  // 如果连接已关闭或已可写，则无需等待
  return connection_->should_close_ || connection_->write_ready_;
}

/**
 * @brief 挂起协程以等待可写事件
 *
 * 当 await_ready() 返回 false 时调用此方法。
 *
 * ## 挂起流程
 *
 * 1. 再次检查条件（防止竞态条件）
 * 2. 如果条件已满足，返回 false 取消挂起
 * 3. 否则保存协程句柄到 write_waiter_，返回 true 挂起协程
 *
 * ## 与 ReadableAwaiter 的区别
 *
 * - ReadableAwaiter 等待 socket 有数据可读
 * - WritableAwaiter 等待 socket 发送缓冲区有空间
 *
 * 通常 socket 总是可写的（除非发送缓冲区满），所以 WritableAwaiter
 * 大多数情况下会立即返回，只有在发送大量数据时才需要等待。
 *
 * @param handle 要挂起的协程句柄
 * @return true 成功挂起协程，等待后续恢复
 * @return false 取消挂起，协程立即继续执行
 */
bool Connection::WritableAwaiter::await_suspend(
    std::coroutine_handle<> handle) noexcept {
  // 再次检查条件，防止竞态条件
  if (connection_->should_close_ || connection_->write_ready_) {
    return false;  // 取消挂起
  }
  // 保存协程句柄，等待 NotifyWritable() 恢复
  connection_->write_waiter_ = handle;
  connection_->state_ = State::kWriting;
  connection_->ArmWriteDeadline();
  return true;  // 真正挂起协程
}

/**
 * @brief 协程恢复后执行的操作
 *
 * 当协程从挂起状态恢复时调用，用于清理状态。
 *
 * ## 实现逻辑
 *
 * 重置 write_ready_ 标志，表示已处理了当前的可写事件。
 * 这确保下一次 co_await 会正确等待新的事件。
 */
void Connection::WritableAwaiter::await_resume() noexcept {
  // 清除写就绪标志
  connection_->write_ready_ = false;
}

// ============================================================================
// 同步服务方法实现
// ============================================================================

/**
 * @brief 同步服务循环
 *
 * 在阻塞模式下服务客户端（内部仍使用非阻塞 I/O）。
 */
bool Connection::Serve() {
  // 确保 socket 为非阻塞模式
  const int flags = ::fcntl(fd_.Get(), F_GETFL, 0);
  if (flags < 0) {
    common::LogError(std::string("fcntl(F_GETFL) failed: ") +
                     std::strerror(errno));
    return false;
  }
  if ((flags & O_NONBLOCK) == 0 &&
      ::fcntl(fd_.Get(), F_SETFL, flags | O_NONBLOCK) < 0) {
    common::LogError(std::string("fcntl(F_SETFL, O_NONBLOCK) failed: ") +
                     std::strerror(errno));
    return false;
  }

  // 主服务循环
  while (true) {
    std::string event_error;

    // 处理读事件
    if (!OnReadable(&event_error)) {
      common::LogError("connection read failed: " + event_error);
      return false;
    }

    // 检查是否需要关闭
    if (ShouldClose()) {
      return true;  // 正常关闭
    }

    // 如果没有待发送数据，继续读取
    if (!HasPendingWrite()) {
      continue;
    }

    // 处理写事件
    if (!OnWritable(&event_error)) {
      common::LogError("connection write failed: " + event_error);
      return false;
    }
  }
}

// ============================================================================
// 内部方法实现 - Socket I/O
// ============================================================================

/**
 * @brief 从 socket 读取数据到读缓冲区
 *
 * 循环读取直到没有更多数据或发生错误。
 */
Connection::ReadResult Connection::ReadFromSocket(std::string* error_msg) {
  char chunk[4096];

  while (true) {
    const ssize_t rc = ::recv(fd_.Get(), chunk, sizeof(chunk), 0);

    if (rc > 0) {
      read_buffer_.append(chunk, static_cast<std::size_t>(rc));
      continue;
    }

    if (rc == 0) {
      if (error_msg != nullptr) {
        *error_msg = "peer closed connection";
      }
      return ReadResult::kPeerClosed;
    }

    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return read_buffer_.readable_bytes() == 0 ? ReadResult::kWouldBlock
                                                : ReadResult::kData;
    }

    if (error_msg != nullptr) {
      *error_msg = std::string("recv failed: ") + std::strerror(errno);
    }
    return ReadResult::kError;
  }
}

// ============================================================================
// 内部方法实现 - 请求解析
// ============================================================================

/**
 * @brief 尝试从读缓冲区解析并处理请求
 *
 * 循环解析缓冲区中所有完整的帧。
 */
bool Connection::TryParseRequests(std::string* error_msg) {
  // 循环处理缓冲区中所有完整的帧。
  // peek() 始终指向当前可读数据的起始位置（连续内存）。
  while (read_buffer_.readable_bytes() >= kFrameHeaderBytes) {
    // 解析帧头获取帧体长度
    std::size_t body_length = 0;
    if (!DecodeFrameHeader(read_buffer_.peek(), &body_length, error_msg)) {
      return false;  // 帧头无效，关闭连接
    }

    // 检查是否已接收完整的帧体
    if (read_buffer_.readable_bytes() < kFrameHeaderBytes + body_length) {
      // 帧体不完整，等待更多数据
      break;
    }

    // 提取帧体（从 peek() + 帧头 开始，保证连续）
    std::string_view body_view(read_buffer_.peek() + kFrameHeaderBytes,
                               body_length);

    // 解析并处理请求
    rpc::RpcRequest request;
    rpc::RpcResponse response;

    if (!request.ParseFromArray(body_view.data(),
                                static_cast<int>(body_view.size()))) {
      // 解析失败：返回错误响应
      response.set_request_id("");
      response.set_error_code(rpc::PARSE_ERROR);
      response.set_error_msg("failed to parse RpcRequest");
      if (!EnqueueResponse(response, error_msg)) {
        return false;
      }
      // 消费掉整个帧（帧头 + 帧体）
      read_buffer_.retrieve(kFrameHeaderBytes + body_length);
    } else {
      // 心跳请求快速路径：不进线程池、不进服务注册表，直接返回空响应。
      if (request.service_name() == kHeartbeatServiceName) {
        rpc::RpcResponse hb_response;
        hb_response.set_request_id(request.request_id());
        hb_response.set_error_code(rpc::OK);
        // 空 payload，最小化网络开销
        if (!EnqueueResponse(hb_response, error_msg)) {
          return false;
        }
        read_buffer_.retrieve(kFrameHeaderBytes + body_length);
      } else if (request_dispatcher_) {
        DispatchRequest dispatch;
        dispatch.sequence = ++next_request_sequence_;
        dispatch.request_id = request.request_id();
        dispatch.service_name = request.service_name();
        dispatch.method_name = request.method_name();
        dispatch.payload = request.payload();
        if (!request_dispatcher_(std::move(dispatch), error_msg)) {
          if (error_msg != nullptr && error_msg->empty()) {
            *error_msg = "failed to dispatch request";
          }
          return false;
        }
        read_buffer_.retrieve(kFrameHeaderBytes + body_length);
      } else {
        // 解析成功：处理请求
        if (!HandleOneRequest(request, &response)) {
          if (error_msg != nullptr) {
            *error_msg = "failed to handle request";
          }
          return false;
        }
        if (!EnqueueResponse(response, error_msg)) {
          return false;
        }
        read_buffer_.retrieve(kFrameHeaderBytes + body_length);
      }
    }
  }

  return true;
}

// ============================================================================
// 内部方法实现 - 请求处理
// ============================================================================

/**
 * @brief 处理单个 RPC 请求
 *
 * 查找处理函数并调用，处理各种异常情况。
 */
bool Connection::HandleOneRequest(const rpc::RpcRequest& request,
                                  rpc::RpcResponse* response) const {
  if (response == nullptr) {
    return false;
  }

  // 初始化响应字段
  response->set_request_id(request.request_id());
  response->set_error_code(rpc::OK);
  response->clear_error_msg();
  response->clear_payload();

  // 查找处理函数
  const auto handler =
      registry_.Find(request.service_name(), request.method_name());

  if (!handler.has_value()) {
    // 方法不存在
    response->set_error_code(rpc::METHOD_NOT_FOUND);
    response->set_error_msg("method not found: " + request.service_name() +
                            "." + request.method_name());
    return true;  // 返回 true 表示处理完成（返回错误响应）
  }

  // 调用处理函数
  try {
    // 调用用户注册的处理函数
    response->set_payload(handler->get()(request.payload()));
    return true;
  } catch (const RpcError& ex) {
    // RPC 业务错误
    response->set_error_code(static_cast<rpc::ErrorCode>(common::ToProtoErrorCode(ex.code())));
    response->set_error_msg(ex.what());
    return true;
  } catch (const std::exception& ex) {
    // 标准异常
    response->set_error_code(static_cast<rpc::ErrorCode>(common::ToProtoErrorCode(
        common::make_error_code(common::ErrorCode::kInternalError))));
    response->set_error_msg(std::string("handler exception: ") + ex.what());
    return true;
  } catch (...) {
    // 未知异常
    response->set_error_code(static_cast<rpc::ErrorCode>(common::ToProtoErrorCode(
        common::make_error_code(common::ErrorCode::kInternalError))));
    response->set_error_msg("handler threw unknown exception");
    return true;
  }
}

// ============================================================================
// 内部方法实现 - 响应发送
// ============================================================================

/**
 * @brief 将响应加入写缓冲区
 *
 * 序列化响应并编码为帧格式。
 */
bool Connection::QueueResponse(const rpc::RpcResponse& response,
                               std::string* error_msg) {
  // 检查响应大小
  const std::size_t body_length =
      static_cast<std::size_t>(response.ByteSizeLong());
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid response frame size";
    }
    return false;
  }

  // 序列化响应
  std::string body;
  body.resize(body_length);
  if (!response.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize RpcResponse";
    }
    return false;
  }

  // 编码帧
  std::string frame;
  if (!EncodeFrame(body, &frame, error_msg)) {
    return false;
  }

  // 背压检查：使用 pending_write_bytes_ 替代 write_buffer_.size()
  const std::size_t next_size = pending_write_bytes_ + frame.size();
  if (next_size > options_.max_write_buffer_bytes) {
    if (error_msg != nullptr) {
      *error_msg = "write buffer backpressure limit exceeded";
    }
    return false;
  }

  // 加入写缓冲区（O(1) 操作）
  pending_write_bytes_ += frame.size();
  write_buffer_.push_back({std::move(frame), 0});
  return true;
}

/**
 * @brief 发送写缓冲区中的数据
 *
 * 非阻塞地发送尽可能多的数据。
 */
bool Connection::FlushWrites(std::string* error_msg) {
  while (!write_buffer_.empty()) {
    auto& chunk = write_buffer_.front();
    const std::size_t remaining = chunk.data.size() - chunk.offset;

    // 尝试发送数据
    // MSG_NOSIGNAL：防止对端关闭时产生 SIGPIPE 信号
    const ssize_t rc = ::send(fd_.Get(), chunk.data.data() + chunk.offset,
                              remaining, MSG_NOSIGNAL);

    if (rc > 0) {
      // 成功发送了 rc 字节，更新游标
      chunk.offset += static_cast<std::size_t>(rc);
      pending_write_bytes_ -= static_cast<std::size_t>(rc);

      // 如果当前块全部发完，弹出
      if (chunk.offset == chunk.data.size()) {
        write_buffer_.pop_front();
      }
      continue;
    }

    if (rc < 0 && errno == EINTR) {
      // 被信号中断，重试
      continue;
    }
    if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 发送缓冲区满，等待下次可写事件
      break;
    }
    // 发送错误
    if (error_msg != nullptr) {
      *error_msg = std::string("send failed: ") + std::strerror(errno);
    }
    return false;
  }

  return true;
}

// ============================================================================
// Buffer 实现
// ============================================================================

void Connection::Buffer::append(const char* data, std::size_t len) {
  ensure_writable(len);
  std::memcpy(begin() + write_index_, data, len);
  write_index_ += len;
}

void Connection::Buffer::compact() {
  if (read_index_ == 0) {
    return;  // 已在头部，无需搬移
  }
  const std::size_t readable = write_index_ - read_index_;
  if (readable > 0) {
    std::memmove(begin(), begin() + read_index_, readable);
  }
  write_index_ = readable;
  read_index_ = 0;
}

void Connection::Buffer::ensure_writable(std::size_t len) {
  if (writable_bytes() >= len) {
    return;  // 尾部空间足够
  }

  // 尾部空间不够，不管是否需要真正扩容，都先把存留数据挪到最前面
  // 这样能最大化利用现有容量，也能统一游标坐标
  compact();

  // 如果 compact 之后，后面的可写空间还是不够，就只能扩容了
  if (writable_bytes() < len) {
    // 注意扩容后整体 size 的设定：现有数据的大小 (write_index_) +
    // 新数据需要的长度 (len)
    buffer_.resize(write_index_ + len);
  }
}

// ============================================================================
// 静态辅助方法实现 - 帧编解码
// ============================================================================

/**
 * @brief 编码帧
 *
 * 在消息体前添加 4 字节大端序长度前缀。
 */
bool Connection::EncodeFrame(const std::string& body, std::string* frame,
                             std::string* error_msg) {
  // 参数检查
  if (frame == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "frame output is null";
    }
    return false;
  }

  // 大小检查
  if (body.empty() || body.size() > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid frame body size";
    }
    return false;
  }

  // 转换为大端序长度
  const std::uint32_t be_length =
      htonl(static_cast<std::uint32_t>(body.size()));

  // 构建完整帧：长度前缀 + 消息体
  frame->clear();
  frame->reserve(kFrameHeaderBytes + body.size());
  frame->append(reinterpret_cast<const char*>(&be_length), kFrameHeaderBytes);
  frame->append(body);
  return true;
}

/**
 * @brief 解码帧头
 *
 * 从缓冲区中读取 4 字节大端序长度前缀。
 */
bool Connection::DecodeFrameHeader(const char* data, std::size_t* body_length,
                                   std::string* error_msg) {
  if (body_length == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "body_length is null";
    }
    return false;
  }

  std::uint32_t be_length = 0;
  std::memcpy(&be_length, data, kFrameHeaderBytes);
  const std::uint32_t parsed = ntohl(be_length);

  if (parsed == 0 || parsed > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid request frame length";
    }
    return false;
  }

  *body_length = static_cast<std::size_t>(parsed);
  return true;
}

}  // namespace rpc::server
