/**
 * @file rpc_server.cpp
 * @brief RPC 服务器核心模块实现
 *
 * 本文件实现了 RpcServer 类的所有方法，包括：
 * - 服务器初始化
 * - Socket 创建与配置
 * - 端口绑定与监听
 * - 客户端连接接受与处理
 *
 * @see rpc_server.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/rpc_server.h"

// 网络相关头文件
#include <arpa/inet.h>   // inet_ntop, htonl, ntohl
#include <netinet/in.h>  // sockaddr_in, INADDR_ANY
#include <sys/socket.h>  // socket, bind, listen, accept, setsockopt

// 标准库头文件
#include <cerrno>   // errno 错误码
#include <cstring>  // strerror
#include <string>   // std::string
#include <utility>  // std::move

// 项目内部头文件
#include "common/log.h"         // 日志输出
#include "common/unique_fd.h"   // RAII 文件描述符包装
#include "server/connection.h"  // 客户端连接处理

namespace rpc::server {

// ============================================================================
// 构造函数实现
// ============================================================================

/**
 * @brief 构造函数实现
 *
 * 初始化服务器配置，保存端口号和服务注册表引用。
 *
 * @note 构造函数只保存配置，不进行实际的 socket 创建
 * @note 实际的网络初始化在 Start() 方法中进行
 */
RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port),           // 保存监听端口
      registry_(registry) {  // 保存服务注册表引用
}

// ============================================================================
// 服务器启动实现
// ============================================================================

/**
 * @brief 启动服务器实现
 *
 * 完整的服务器启动流程：
 *
 * 1. **创建 Socket**
 *    - 使用 IPv4 (AF_INET)
 *    - 使用 TCP 协议 (SOCK_STREAM)
 *
 * 2. **设置 Socket 选项**
 *    - SO_REUSEADDR: 允许重用处于 TIME_WAIT 状态的地址
 *    - 这解决了服务重启时 "Address already in use" 的问题
 *
 * 3. **绑定地址**
 *    - 绑定到所有网络接口 (INADDR_ANY)
 *    - 使用指定的端口号
 *
 * 4. **开始监听**
 *    - 设置连接队列长度为 128
 *    - 等待客户端连接
 *
 * 5. **接受连接循环**
 *    - 阻塞等待客户端连接
 *    - 为每个连接创建 Connection 对象
 *    - 处理连接直到关闭
 *
 * @return true 启动成功（通常不会返回）
 * @return false 启动失败
 *
 * ## 错误处理
 *
 * | 阶段 | 错误原因 | 处理方式 |
 * |------|----------|----------|
 * | socket() | 系统资源不足 | 返回 false |
 * | setsockopt() | 无效选项 | 返回 false |
 * | bind() | 端口被占用 | 返回 false |
 * | listen() | 系统资源不足 | 返回 false |
 * | accept() | 信号中断 | 继续等待 |
 * | accept() | 其他错误 | 记录日志，继续等待 |
 */
bool RpcServer::Start() {
  // ===== 步骤 1：创建监听 Socket =====
  // AF_INET: IPv4 协议族
  // SOCK_STREAM: 面向连接的 TCP 协议
  // 0: 自动选择协议（对于 SOCK_STREAM 会选择 TCP）
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    // socket 创建失败，通常是因为系统资源不足
    common::LogError(std::string("socket failed: ") + std::strerror(errno));
    return false;
  }

  // ===== 步骤 2：设置 SO_REUSEADDR 选项 =====
  // SO_REUSEADDR 允许重用处于 TIME_WAIT 状态的地址
  // 这解决了服务快速重启时 "Address already in use" 的问题
  //
  // TCP 连接关闭后会进入 TIME_WAIT 状态（持续约 2*MSL 时间）
  // 如果不设置此选项，在这段时间内无法重新绑定同一端口
  int reuse = 1;
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    common::LogError(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                     std::strerror(errno));
    return false;
  }

  // ===== 步骤 3：绑定地址和端口 =====
  // 配置服务器地址结构
  sockaddr_in addr{};
  addr.sin_family = AF_INET;                 // IPv4
  addr.sin_port = htons(port_);              // 端口号（转换为网络字节序）
  addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 绑定到所有网络接口

  // 执行绑定
  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    // 绑定失败，最常见的原因是端口已被占用
    common::LogError(std::string("bind failed: ") + std::strerror(errno));
    return false;
  }

  // ===== 步骤 4：开始监听 =====
  // backlog = 128: 等待队列的最大长度
  // 超过此长度的连接将被拒绝（客户端会收到 ECONNREFUSED）
  if (::listen(listen_fd.Get(), 128) < 0) {
    common::LogError(std::string("listen failed: ") + std::strerror(errno));
    return false;
  }

  // 服务器启动成功，记录日志
  common::LogInfo("RPC server listening on port " + std::to_string(port_));

  // ===== 步骤 5：接受连接循环 =====
  // 这是一个无限循环，持续接受客户端连接
  while (true) {
    // 准备接受客户端地址信息
    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);

    // 阻塞等待客户端连接
    // accept() 会返回一个新的 socket 描述符用于与客户端通信
    // listen_fd 继续用于接受新连接
    // 每个连接 fd 也由 RAII 托管，离开循环体自动释放
    common::UniqueFd client_fd(
        ::accept(listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
                 &client_addr_len));

    if (!client_fd) {
      // accept 失败
      if (errno == EINTR) {
        // 被信号中断，继续等待新连接
        continue;
      }
      // 其他错误，记录日志并继续运行
      common::LogError(std::string("accept failed: ") + std::strerror(errno));
      continue;
    }

    // 获取客户端地址信息用于日志记录
    // inet_ntop: 将二进制 IP 地址转换为点分十进制字符串
    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));

    // 记录客户端连接信息
    common::LogInfo(std::string("client connected: ") + ip + ':' +
                    std::to_string(ntohs(client_addr.sin_port)));

    // ===== 步骤 6：处理客户端连接 =====
    // 创建 Connection 对象处理此连接
    // 注意：这是阻塞式处理，一次只处理一个客户端
    Connection connection(std::move(client_fd), registry_);

    // 运行连接服务循环（阻塞直到连接关闭）
    // 返回值表示连接是否正常关闭，这里忽略返回值
    // 因为我们总是继续接受新连接
    (void)connection.Serve();

    // 连接处理完成，记录日志
    common::LogInfo("client disconnected");

    // Connection 对象在此处销毁，client_fd 自动关闭
  }
}

}  // namespace rpc::server