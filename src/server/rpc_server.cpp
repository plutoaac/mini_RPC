#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "common/log.h"
#include "common/unique_fd.h"
#include "coroutine/task.h"
#include "server/connection.h"

namespace rpc::server {

namespace {

/**
 * @struct ConnectionState
 * @brief 单个客户端连接的完整状态
 * 
 * 封装了处理一个客户端连接所需的所有状态：
 * - Connection 对象：负责 socket I/O 操作
 * - 协程任务：异步处理该连接的协程
 * - 状态标志：记录协程的执行状态
 */
struct ConnectionState {
  /// 构造函数：初始化 Connection，协程需要后续通过 task.emplace() 启动
  ConnectionState(common::UniqueFd fd, const ServiceRegistry& registry)
      : connection(std::move(fd), registry) {}

  /// Connection 对象：封装 socket I/O 和协程等待机制
  Connection connection;
  
  /// 协程任务：存储 HandleConnectionCo() 协程，Task 存在则协程存在
  std::optional<rpc::coroutine::Task<void>> task;
  
  /// 协程状态标志：false 表示因错误退出
  bool coroutine_ok{true};
  
  /// 错误信息：协程出错时存储具体错误
  std::string coroutine_error;
};

/**
 * @brief 处理单个客户端连接的协程函数
 * 
 * 核心协程，在无限循环中：读取请求 → 处理请求 → 发送响应
 * 
 * ## 两个挂起点
 * - co_await ReadRequestCo()：等待 socket 可读
 * - co_await WriteResponseCo()：等待 socket 可写
 */
rpc::coroutine::Task<void> HandleConnectionCo(Connection* connection,
                                              bool* coroutine_ok,
                                              std::string* coroutine_error) {
  // 主循环：持续处理请求直到连接关闭
  while (true) {
    // 步骤1：检查连接是否应该关闭
    if (connection->ShouldClose()) {
      co_return;
    }

    // 步骤2：异步读取并处理请求（协程在此处可能挂起）
    const bool read_ok = co_await connection->ReadRequestCo(coroutine_error);
    if (!read_ok) {
      *coroutine_ok = false;
      co_return;
    }

    // 步骤3：再次检查连接状态
    if (connection->ShouldClose()) {
      co_return;
    }

    // 步骤4：发送所有待发送的响应数据
    while (connection->HasPendingWrite()) {
      // 协程在此处可能挂起，等待 socket 可写
      const bool write_ok =
          co_await connection->WriteResponseCo(coroutine_error);
      if (!write_ok) {
        *coroutine_ok = false;
        co_return;
      }

      if (connection->ShouldClose()) {
        co_return;
      }
    }
  }
}

}  // namespace

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port), registry_(registry) {}

bool RpcServer::Start() {
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    common::LogError(std::string("socket failed: ") + std::strerror(errno));
    return false;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    common::LogError(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                     std::strerror(errno));
    return false;
  }

  const int listen_flags = ::fcntl(listen_fd.Get(), F_GETFL, 0);
  if (listen_flags < 0 ||
      ::fcntl(listen_fd.Get(), F_SETFL, listen_flags | O_NONBLOCK) < 0) {
    common::LogError(std::string("fcntl(O_NONBLOCK) failed for listen fd: ") +
                     std::strerror(errno));
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    common::LogError(std::string("bind failed: ") + std::strerror(errno));
    return false;
  }

  if (::listen(listen_fd.Get(), 128) < 0) {
    common::LogError(std::string("listen failed: ") + std::strerror(errno));
    return false;
  }

  common::UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    common::LogError(std::string("epoll_create1 failed: ") +
                     std::strerror(errno));
    return false;
  }

  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;
  listen_ev.data.fd = listen_fd.Get();
  if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_ADD, listen_fd.Get(), &listen_ev) <
      0) {
    common::LogError(std::string("epoll_ctl add listen fd failed: ") +
                     std::strerror(errno));
    return false;
  }

  common::LogInfo("RPC server listening on port " + std::to_string(port_));

  std::unordered_map<int, std::unique_ptr<ConnectionState>> connections;
  epoll_event events[64] = {};

  const auto close_connection = [&](int fd, const std::string& reason) {
    const auto it = connections.find(fd);
    if (it == connections.end()) {
      return;
    }

    ConnectionState& state = *it->second;
    state.connection.MarkClosing();
    state.connection.NotifyReadable();
    state.connection.NotifyWritable();
    if (state.task.has_value()) {
      state.task->Get();
      state.task.reset();
    }

    common::LogInfo("closing client fd=" + std::to_string(fd) +
                    " reason=" + reason);
    (void)::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_DEL, fd, nullptr);
    connections.erase(it);
  };

  while (true) {
    const int ready = ::epoll_wait(epoll_fd.Get(), events, 64, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      common::LogError(std::string("epoll_wait failed: ") +
                       std::strerror(errno));
      return false;
    }

    for (int i = 0; i < ready; ++i) {
      const int fd = events[i].data.fd;
      const std::uint32_t ev = events[i].events;

      if (fd == listen_fd.Get()) {
        while (true) {
          sockaddr_in client_addr{};
          socklen_t client_addr_len = sizeof(client_addr);
          const int client_raw_fd = ::accept4(
              listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
              &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client_raw_fd < 0) {
            if (errno == EINTR) {
              continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            common::LogError(std::string("accept4 failed: ") +
                             std::strerror(errno));
            break;
          }

          char ip[INET_ADDRSTRLEN] = {0};
          ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
          common::LogInfo(std::string("client connected: ") + ip + ':' +
                          std::to_string(ntohs(client_addr.sin_port)));

          epoll_event client_ev{};
          client_ev.events = EPOLLIN | EPOLLRDHUP;
          client_ev.data.fd = client_raw_fd;
          if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_ADD, client_raw_fd,
                          &client_ev) < 0) {
            common::LogError(std::string("epoll_ctl add client fd failed: ") +
                             std::strerror(errno));
            ::close(client_raw_fd);
            continue;
          }

          // =====================================================================
          // 创建连接状态并启动协程
          // =====================================================================
          
          // 步骤1：创建 ConnectionState 对象
          // - 将 client_raw_fd 封装为 UniqueFd（RAII 管理文件描述符）
          // - 传入 registry_ 引用，用于查找 RPC 方法处理函数
          // - 使用 make_unique 在堆上分配，因为连接状态需要在事件循环中长期存在
          auto state = std::make_unique<ConnectionState>(
              common::UniqueFd(client_raw_fd), registry_);
          
          // 步骤2：启动处理该连接的协程
          // - task 是 std::optional<Task<void>>，初始为空
          // - emplace() 在 optional 内部原地构造 Task 对象
          // - HandleConnectionCo() 返回 Task，代表协程的执行
          // 
          // 重要：协程在这里启动，但不会立即执行完！
          // - 协程开始执行，遇到第一个 co_await 时会挂起
          // - 挂起后，控制权返回到这里，继续执行下面的代码
          // - 协程的后续执行由事件循环驱动（通过 NotifyXxx 恢复）
          //
          // 参数说明：
          // - &state->connection：Connection 对象指针，用于 I/O 操作
          // - &state->coroutine_ok：输出参数，记录协程是否正常
          // - &state->coroutine_error：输出参数，记录错误信息
          state->task.emplace(HandleConnectionCo(&state->connection,
                                                 &state->coroutine_ok,
                                                 &state->coroutine_error));

          // 步骤3：将连接状态存入 connections map
          // - key：client_raw_fd（socket 文件描述符）
          // - value：unique_ptr<ConnectionState>（连接状态的所有权转移到 map）
          // 
          // 结构化绑定：
          // - it：指向插入位置的迭代器（或已存在元素的迭代器）
          // - inserted：是否插入成功（true=新插入，false=key已存在）
          //
          // 注意：std::move(state) 后，state 变量不再拥有对象所有权
          const auto [it, inserted] =
              connections.emplace(client_raw_fd, std::move(state));
          if (!inserted) {
            common::LogError("duplicate client fd in connection map");
            (void)::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_DEL, client_raw_fd,
                              nullptr);
            ::close(client_raw_fd);
          }
        }
        continue;
      }

      const auto it = connections.find(fd);
      if (it == connections.end()) {
        continue;
      }

      ConnectionState& state = *it->second;

      if ((ev & EPOLLIN) != 0U) {
        state.connection.NotifyReadable();
      }
      if ((ev & EPOLLOUT) != 0U) {
        state.connection.NotifyWritable();
      }
      if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        state.connection.MarkClosing();
        state.connection.NotifyReadable();
        state.connection.NotifyWritable();
      }

      if (!state.coroutine_ok) {
        close_connection(fd, state.coroutine_error.empty()
                                 ? "connection coroutine failed"
                                 : state.coroutine_error);
        continue;
      }

      if (state.connection.ShouldClose()) {
        close_connection(fd, "peer closed or marked closing");
        continue;
      }

      epoll_event next_ev{};
      next_ev.events = EPOLLIN | EPOLLRDHUP;
      if (state.connection.HasPendingWrite()) {
        next_ev.events |= EPOLLOUT;
      }
      next_ev.data.fd = fd;
      if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_MOD, fd, &next_ev) < 0) {
        close_connection(
            fd, std::string("epoll_ctl mod failed: ") + std::strerror(errno));
      }
    }
  }
}

}  // namespace rpc::server
