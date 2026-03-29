/**
 * @file rpc_server.cpp
 * @brief RPC 服务端核心模块实现
 * 
 * 本文件实现了 RpcServer 类的所有方法，包括：
 * - 服务端初始化（socket 创建、绑定、监听）
 * - epoll 事件循环
 * - 连接接入与管理
 * 
 * ## 实现要点
 * 
 * 1. **非阻塞 I/O**
 *    - 监听 socket 设置为非阻塞模式
 *    - 使用 accept4 创建非阻塞客户端 socket
 *    - 所有 I/O 操作都不会阻塞事件循环
 * 
 * 2. **epoll 事件处理**
 *    - EPOLLIN：可读事件（新连接或数据到达）
 *    - EPOLLOUT：可写事件（发送缓冲区可写）
 *    - EPOLLRDHUP：对端关闭连接
 *    - EPOLLERR/EPOLLHUP：错误事件
 * 
 * 3. **资源管理**
 *    - 使用 UniqueFd RAII 管理文件描述符
 *    - 使用 unordered_map 管理连接生命周期
 * 
 * @see rpc_server.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <tuple>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/log.h"
#include "common/unique_fd.h"
#include "server/connection.h"

namespace rpc::server {

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port), registry_(registry) {}

bool RpcServer::Start() {
  // =========================================================================
  // 阶段 1：创建监听 socket
  // =========================================================================
  
  // 创建 TCP socket（AF_INET: IPv4, SOCK_STREAM: TCP）
  // 使用 RAII 包装器管理 fd，函数退出时自动 close
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    common::LogError(std::string("socket failed: ") + std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 2：设置 socket 选项
  // =========================================================================
  
  // 设置 SO_REUSEADDR 选项
  // 作用：允许重启服务时立即重用处于 TIME_WAIT 状态的地址
  // 场景：服务重启时，之前监听的端口可能还处于 TIME_WAIT 状态
  //       不设置此选项可能导致 bind() 失败
  int reuse = 1;
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    common::LogError(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                     std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 3：设置非阻塞模式
  // =========================================================================
  
  // 将监听 socket 设置为非阻塞模式
  // 原因：accept 循环需要能够一次性清空所有待处理的连接
  //       非阻塞模式下，当没有更多连接时 accept 返回 EAGAIN
  const int listen_flags = ::fcntl(listen_fd.Get(), F_GETFL, 0);
  if (listen_flags < 0 ||
      ::fcntl(listen_fd.Get(), F_SETFL, listen_flags | O_NONBLOCK) < 0) {
    common::LogError(std::string("fcntl(O_NONBLOCK) failed for listen fd: ") +
                     std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 4：绑定地址和端口
  // =========================================================================
  
  // 配置监听地址结构
  sockaddr_in addr{};
  addr.sin_family = AF_INET;           // IPv4
  addr.sin_port = htons(port_);        // 端口号（转换为网络字节序）
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网络接口

  // 绑定 socket 到指定端口
  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    common::LogError(std::string("bind failed: ") + std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 5：开始监听
  // =========================================================================
  
  // 开始监听连接请求
  // backlog=128：内核维护的待处理连接队列的最大长度
  // 实际值可能被 /proc/sys/net/core/somaxconn 限制
  if (::listen(listen_fd.Get(), 128) < 0) {
    common::LogError(std::string("listen failed: ") + std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 6：创建 epoll 实例
  // =========================================================================
  
  // 创建 epoll 实例
  // EPOLL_CLOEXEC：exec 时自动关闭 fd，防止泄漏给子进程
  common::UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd) {
    common::LogError(std::string("epoll_create1 failed: ") +
                     std::strerror(errno));
    return false;
  }

  // =========================================================================
  // 阶段 7：注册监听 socket 到 epoll
  // =========================================================================
  
  // 配置监听 socket 的 epoll 事件
  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;          // 监听可读事件（新连接到达）
  listen_ev.data.fd = listen_fd.Get(); // 保存 fd 以便事件处理时识别
  
  // 将监听 socket 添加到 epoll 实例
  if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_ADD, listen_fd.Get(), &listen_ev) <
      0) {
    common::LogError(std::string("epoll_ctl add listen fd failed: ") +
                     std::strerror(errno));
    return false;
  }

  common::LogInfo("RPC server listening on port " + std::to_string(port_));

  // =========================================================================
  // 阶段 8：初始化连接管理数据结构
  // =========================================================================
  
  // 连接映射表：fd -> Connection
  // 使用 unordered_map 实现 O(1) 平均查找时间
  std::unordered_map<int, Connection> connections;
  
  // epoll 事件数组，每次 epoll_wait 最多返回 64 个事件
  epoll_event events[64] = {};

  // =========================================================================
  // 辅助函数：关闭连接并清理资源
  // =========================================================================
  
  // 关闭连接的内部辅助函数
  // 执行连接关闭的完整流程：
  // 1. 从 epoll 中删除 fd
  // 2. 从连接映射表中删除（触发 Connection 析构，关闭 fd）
  const auto close_connection = [&](int fd, const std::string& reason) {
    common::LogInfo("closing client fd=" + std::to_string(fd) +
                    " reason=" + reason);
    // 从 epoll 实例中删除该 fd
    // 最后一个参数为 nullptr 表示不需要获取之前注册的事件
    (void)::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_DEL, fd, nullptr);
    // 从映射表中删除，触发 Connection 析构
    // UniqueFd 的析构函数会自动 close fd
    connections.erase(fd);
  };

  // =========================================================================
  // 阶段 9：主事件循环
  // =========================================================================
  
  while (true) {
    // 等待事件发生
    // timeout=-1：无限等待，直到有事件发生
    const int ready = ::epoll_wait(epoll_fd.Get(), events, 64, -1);
    
    // 处理 epoll_wait 返回错误
    if (ready < 0) {
      if (errno == EINTR) {
        // 被信号中断，继续等待
        continue;
      }
      // 其他错误，记录日志并退出
      common::LogError(std::string("epoll_wait failed: ") +
                       std::strerror(errno));
      return false;
    }

    // 处理所有就绪的事件
    for (int i = 0; i < ready; ++i) {
      const int fd = events[i].data.fd;           // 获取事件对应的 fd
      const std::uint32_t ev = events[i].events;  // 获取事件类型

      // ----------------------------------------------------------------------
      // 事件处理分支 1：监听 socket 可读（新连接到达）
      // ----------------------------------------------------------------------
      if (fd == listen_fd.Get()) {
        // 循环 accept 直到没有更多待处理连接
        // 使用非阻塞模式，accept 在没有更多连接时返回 EAGAIN
        while (true) {
          sockaddr_in client_addr{};
          socklen_t client_addr_len = sizeof(client_addr);
          
          // 接受新连接
          // accept4 比 accept 多一个 flags 参数
          // SOCK_NONBLOCK：设置非阻塞模式（无需额外的 fcntl 调用）
          // SOCK_CLOEXEC：exec 时自动关闭 fd
          const int client_raw_fd =
              ::accept4(listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
                        &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
          
          // 处理 accept 错误
          if (client_raw_fd < 0) {
            if (errno == EINTR) {
              // 被信号中断，重试
              continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              // 没有更多待处理的连接，退出 accept 循环
              break;
            }
            // 其他错误，记录日志并退出 accept 循环
            common::LogError(std::string("accept4 failed: ") +
                             std::strerror(errno));
            break;
          }

          // 获取客户端 IP 地址和端口的字符串表示（用于日志）
          char ip[INET_ADDRSTRLEN] = {0};
          ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
          common::LogInfo(std::string("client connected: ") + ip + ':' +
                          std::to_string(ntohs(client_addr.sin_port)));

          // 将新客户端 fd 注册到 epoll
          epoll_event client_ev{};
          // EPOLLIN：监听可读事件（数据到达）
          // EPOLLRDHUP：监听对端关闭连接事件
          client_ev.events = EPOLLIN | EPOLLRDHUP;
          client_ev.data.fd = client_raw_fd;
          
          if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_ADD, client_raw_fd,
                          &client_ev) < 0) {
            common::LogError(std::string("epoll_ctl add client fd failed: ") +
                             std::strerror(errno));
            ::close(client_raw_fd);
            continue;
          }

          // 创建 Connection 对象并存入映射表
          // 使用 piecewise_construct 实现原地构造，避免额外的移动/复制
          const auto [it, inserted] = connections.emplace(
              std::piecewise_construct, 
              std::forward_as_tuple(client_raw_fd),
              std::forward_as_tuple(common::UniqueFd(client_raw_fd), registry_));
          
          if (!inserted) {
            // 理论上不应该发生（fd 应该是唯一的）
            common::LogError("duplicate client fd in connection map");
            (void)::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_DEL, client_raw_fd,
                              nullptr);
            ::close(client_raw_fd);
          }
        }
        continue;  // 处理下一个事件
      }

      // ----------------------------------------------------------------------
      // 事件处理分支 2：客户端 socket 事件
      // ----------------------------------------------------------------------
      
      // 查找对应的 Connection 对象
      const auto it = connections.find(fd);
      if (it == connections.end()) {
        // 找不到连接对象，忽略此事件
        // 这可能发生在连接刚被关闭但还有未处理的事件
        continue;
      }

      Connection& connection = it->second;
      bool ok = true;
      std::string conn_error;

      // 处理可读事件（数据到达或对端关闭）
      if ((ev & EPOLLIN) != 0U) {
        ok = connection.OnReadable(&conn_error);
      }
      
      // 处理可写事件（发送缓冲区可写）
      // 只有当 OnReadable 成功时才处理写事件
      if (ok && (ev & EPOLLOUT) != 0U) {
        ok = connection.OnWritable(&conn_error);
      }

      // 检查错误/挂起/对端关闭事件
      // EPOLLERR：socket 发生错误
      // EPOLLHUP：挂起事件（通常是对端关闭）
      // EPOLLRDHUP：对端关闭连接（Linux 2.6.17+）
      if ((ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        connection.MarkClosing();
      }

      // 处理 I/O 错误
      if (!ok) {
        close_connection(fd, conn_error.empty() ? "io error" : conn_error);
        continue;
      }
      
      // 检查是否需要关闭连接
      if (connection.ShouldClose()) {
        close_connection(fd, "peer closed or marked closing");
        continue;
      }

      // 更新 epoll 事件注册
      // 根据是否有待发送数据动态调整监听的事件
      epoll_event next_ev{};
      next_ev.events = EPOLLIN | EPOLLRDHUP;  // 始终监听读事件和对端关闭
      if (connection.HasPendingWrite()) {
        // 有待发送数据，监听可写事件
        next_ev.events |= EPOLLOUT;
      }
      next_ev.data.fd = fd;
      
      // 使用 EPOLL_CTL_MOD 修改已注册的事件
      if (::epoll_ctl(epoll_fd.Get(), EPOLL_CTL_MOD, fd, &next_ev) < 0) {
        close_connection(fd,
                         std::string("epoll_ctl mod failed: ") +
                             std::strerror(errno));
      }
    }
  }
}

}  // namespace rpc::server
