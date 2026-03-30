/**
 * @file worker_loop.cpp
 * @brief WorkerLoop 实现 - 多线程 RPC 服务的工作线程事件循环
 *
 * WorkerLoop 是多线程 RPC 服务器的核心组件：
 * - 每个 WorkerLoop 在独立的线程中运行
 * - 主线程 accept 连接后，将连接分发给 WorkerLoop
 * - WorkerLoop 使用 epoll + 协程处理多个连接
 *
 * ## 架构
 *
 * ```
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                      Main Thread                            │
 *   │  listen_fd ──> accept() ──> 分发连接给 WorkerLoop          │
 *   └─────────────────────────────────────────────────────────────┘
 *                              │
 *          ┌───────────────────┼───────────────────┐
 *          ▼                   ▼                   ▼
 *   ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
 *   │ WorkerLoop 0│     │ WorkerLoop 1│     │ WorkerLoop N│
 *   │  epoll +    │     │  epoll +    │     │  epoll +    │
 *   │  协程       │     │  协程       │     │  协程       │
 *   └─────────────┘     └─────────────┘     └─────────────┘
 * ```
 *
 * ## 与 RpcServer::Start() 的区别
 *
 * RpcServer::Start() 是单线程版本，所有连接在主线程处理。
 * WorkerLoop 是多线程版本，连接分布到多个工作线程。
 *
 * @see worker_loop.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/worker_loop.h"

#include <sys/epoll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include "common/log.h"

namespace rpc::server {

namespace {

/// 每次 epoll_wait 返回的最大事件数
constexpr int kMaxEventsPerPoll = 64;

/**
 * @brief 处理单个客户端连接的协程函数
 *
 * 这是服务端的核心协程，在无限循环中：读取请求 → 处理请求 → 发送响应
 *
 * ## 协程的挂起点
 *
 * 1. `co_await ReadRequestCo()` - 等待 socket 可读
 * 2. `co_await WriteResponseCo()` - 等待 socket 可写
 *
 * ## 协程的生命周期
 *
 * ```
 *   AddConnection() ──> 创建 ConnectionState
 *                       启动 HandleConnectionCo 协程
 *                              │
 *                              ▼
 *                       协程挂起等待 I/O
 *                              │
 *                              ▼
 *   PollOnce() ──> epoll_wait() 检测事件
 *                       NotifyXxx() 恢复协程
 *                              │
 *                              ▼
 *                       协程继续执行，处理请求
 *                              │
 *                              ▼
 *   CloseConnection() ──> 协程结束
 *                         清理资源
 * ```
 *
 * @param connection Connection 对象指针
 * @param coroutine_ok 输出参数，记录协程是否正常退出
 * @param coroutine_error 输出参数，记录错误信息
 * @return Task<void> 协程任务
 */
rpc::coroutine::Task<void> HandleConnectionCo(Connection* connection,
                                              bool* coroutine_ok,
                                              std::string* coroutine_error) {
  // 主循环：持续处理请求直到连接关闭
  while (true) {
    // 步骤1：检查连接是否应该关闭
    // 可能在挂起期间被其他地方标记为关闭（如超时、对端关闭）
    if (connection->ShouldClose()) {
      co_return;
    }

    // 步骤2：异步读取并处理请求
    // 协程在此处可能挂起，等待 socket 变为可读
    const bool read_ok = co_await connection->ReadRequestCo(coroutine_error);
    if (!read_ok) {
      // 读取失败，记录错误并退出
      *coroutine_ok = false;
      // 如果 coroutine_error 为空，从 LastError() 获取（超时场景）
      if (coroutine_error->empty()) {
        *coroutine_error = connection->LastError();
      }
      co_return;
    }

    // 步骤3：再次检查连接状态
    // 读取过程中可能发生错误或对端关闭
    if (connection->ShouldClose()) {
      co_return;
    }

    // 步骤4：发送所有待发送的响应数据
    // 可能有多个响应需要发送（批量请求场景）
    while (connection->HasPendingWrite()) {
      // 协程在此处可能挂起，等待 socket 变为可写
      const bool write_ok =
          co_await connection->WriteResponseCo(coroutine_error);
      if (!write_ok) {
        // 发送失败，记录错误并退出
        *coroutine_ok = false;
        if (coroutine_error->empty()) {
          *coroutine_error = connection->LastError();
        }
        co_return;
      }

      // 检查连接是否应该关闭
      if (connection->ShouldClose()) {
        co_return;
      }
    }
  }
}

}  // namespace

// ============================================================================
// 构造函数和初始化
// ============================================================================

WorkerLoop::WorkerLoop(std::size_t worker_id, const ServiceRegistry& registry)
    : worker_id_(worker_id), registry_(registry) {}

bool WorkerLoop::Init(std::string* error_msg) {
  // 创建 epoll 实例
  // EPOLL_CLOEXEC: exec 时自动关闭 fd，防止泄露给子进程
  epoll_fd_ = rpc::common::UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_) {
    if (error_msg != nullptr) {
      *error_msg = std::string("epoll_create1 failed: ") + std::strerror(errno);
    }
    return false;
  }
  return true;
}

// ============================================================================
// 连接管理
// ============================================================================

bool WorkerLoop::AddConnection(rpc::common::UniqueFd fd,
                               std::string_view peer_desc,
                               std::string* error_msg) {
  // 检查 epoll 是否已初始化
  if (!epoll_fd_) {
    if (error_msg != nullptr) {
      *error_msg = "worker loop not initialized";
    }
    return false;
  }

  // 获取原始 fd，后续需要作为 epoll 和 map 的 key
  const int raw_fd = fd.Get();

  // 步骤1：将 socket 注册到 epoll
  epoll_event client_ev{};
  client_ev.events = EPOLLIN | EPOLLRDHUP;  // 监听可读和对端关闭事件
  client_ev.data.fd = raw_fd;
  if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_ADD, raw_fd, &client_ev) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("epoll_ctl add client fd failed: ") +
                   std::strerror(errno);
    }
    return false;
  }

  // 步骤2：创建 ConnectionState 并启动协程
  // - Connection 封装了 socket I/O 和协议处理
  // - Task 存储协程的执行状态
  auto state = std::make_unique<ConnectionState>(std::move(fd), registry_);

  // 启动协程：协程开始执行，遇到第一个 co_await 时挂起
  state->task.emplace(HandleConnectionCo(
      &state->connection, &state->coroutine_ok, &state->coroutine_error));

  // 步骤3：将连接状态存入 map
  const auto [it, inserted] = connections_.emplace(raw_fd, std::move(state));
  if (!inserted) {
    // 极端情况：fd 已存在于 map 中（不应该发生）
    (void)::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_DEL, raw_fd, nullptr);
    if (error_msg != nullptr) {
      *error_msg = "duplicate client fd in worker connection map";
    }
    return false;
  }

  common::LogInfo("worker=" + std::to_string(worker_id_) +
                  " adopted connection " + std::string(peer_desc) +
                  " fd=" + std::to_string(raw_fd));
  return true;
}

// ============================================================================
// 事件循环
// ============================================================================

bool WorkerLoop::PollOnce(int timeout_ms, std::string* error_msg) {
  if (!epoll_fd_) {
    if (error_msg != nullptr) {
      *error_msg = "worker loop not initialized";
    }
    return false;
  }

  // 步骤1：调用 epoll_wait 等待 I/O 事件
  epoll_event events[kMaxEventsPerPoll] = {};
  const int ready =
      ::epoll_wait(epoll_fd_.Get(), events, kMaxEventsPerPoll, timeout_ms);
  if (ready < 0) {
    // EINTR: 被信号中断，不是真正的错误
    if (errno == EINTR) {
      return true;
    }
    if (error_msg != nullptr) {
      *error_msg =
          std::string("worker epoll_wait failed: ") + std::strerror(errno);
    }
    return false;
  }

  // 步骤2：检查所有连接的超时
  // 即使 epoll_wait 超时（ready == 0），也需要检查超时
  const auto now = std::chrono::steady_clock::now();
  for (auto& [fd, state] : connections_) {
    (void)fd;  // 避免未使用变量警告
    // Tick() 会检查读/写超时，超时则调用 EnterError()
    state->connection.Tick(now);
  }

  // 步骤3：处理就绪的 I/O 事件
  for (int i = 0; i < ready; ++i) {
    const int fd = events[i].data.fd;
    const std::uint32_t ev = events[i].events;

    const auto it = connections_.find(fd);
    if (it == connections_.end()) {
      // 连接可能已被其他逻辑关闭
      continue;
    }

    ConnectionState& state = *it->second;

    // 3.1 处理可读事件 EPOLLIN
    // socket 有数据可读，唤醒等待读的协程
    if ((ev & EPOLLIN) != 0U) {
      state.connection.NotifyReadable();
    }

    // 3.2 处理可写事件 EPOLLOUT
    // socket 发送缓冲区有空间，唤醒等待写的协程
    if ((ev & EPOLLOUT) != 0U) {
      state.connection.NotifyWritable();
    }

    // 3.3 处理连接关闭/错误事件
    // EPOLLHUP: 挂起（对端关闭写端）
    // EPOLLRDHUP: 对端关闭连接
    // EPOLLERR: socket 错误
    if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
      state.connection.MarkClosing();
      // 唤醒协程让其检测到关闭状态并退出
      state.connection.NotifyReadable();
      state.connection.NotifyWritable();
    }

    // 3.4 检查协程是否因错误退出
    if (!state.coroutine_ok) {
      const std::string reason = state.coroutine_error.empty()
                                     ? "connection coroutine failed"
                                     : state.coroutine_error;
      if (!CloseConnection(fd, reason)) {
        if (error_msg != nullptr) {
          *error_msg = "failed to close errored connection";
        }
        return false;
      }
      continue;
    }

    // 3.5 检查连接是否应该关闭
    if (state.connection.ShouldClose()) {
      const std::string reason = state.connection.LastError().empty()
                                     ? "peer closed or marked closing"
                                     : state.connection.LastError();
      if (!CloseConnection(fd, reason)) {
        if (error_msg != nullptr) {
          *error_msg = "failed to close closing connection";
        }
        return false;
      }
      continue;
    }

    // 3.6 更新 epoll 事件注册
    // 根据是否有待发送数据，动态调整监听的事件
    epoll_event next_ev{};
    next_ev.events = EPOLLIN | EPOLLRDHUP;
    if (state.connection.HasPendingWrite()) {
      // 有数据待发送，监听可写事件
      next_ev.events |= EPOLLOUT;
    }
    next_ev.data.fd = fd;
    if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_MOD, fd, &next_ev) < 0) {
      const std::string reason =
          std::string("epoll_ctl mod failed: ") + std::strerror(errno);
      if (!CloseConnection(fd, reason)) {
        if (error_msg != nullptr) {
          *error_msg = "failed to close connection after epoll mod failure";
        }
        return false;
      }
    }
  }

  return true;
}

// ============================================================================
// 状态查询
// ============================================================================

std::size_t WorkerLoop::WorkerId() const noexcept { return worker_id_; }

std::size_t WorkerLoop::ConnectionCount() const noexcept {
  return connections_.size();
}

// ============================================================================
// 连接关闭
// ============================================================================

bool WorkerLoop::CloseConnection(int fd, std::string_view reason) {
  const auto it = connections_.find(fd);
  if (it == connections_.end()) {
    return true;
  }

  ConnectionState& state = *it->second;

  // 步骤1：标记连接为关闭中
  state.connection.MarkClosing();

  // 步骤2：唤醒所有等待的协程
  // 协程检测到 ShouldClose() 后会正常退出
  state.connection.NotifyReadable();
  state.connection.NotifyWritable();

  // 步骤3：等待协程结束
  // Get() 会阻塞直到协程执行完毕（如果协程还在运行）
  // 通常协程在 Notify 后会很快检测到关闭
  if (state.task.has_value()) {
    (void)state.task->Get();
  }

  // 步骤4：从 epoll 移除 fd
  (void)::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_DEL, fd, nullptr);

  // 步骤5：从 map 移除连接状态
  // 这会触发 ConnectionState 析构，关闭 socket
  connections_.erase(it);

  common::LogInfo("worker=" + std::to_string(worker_id_) +
                  " closed connection fd=" + std::to_string(fd) +
                  " reason: " + std::string(reason));
  return true;
}

}  // namespace rpc::server
