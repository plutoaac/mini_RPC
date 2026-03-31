/**
 * @file rpc_server.h
 * @brief RPC 服务端核心模块
 *
 * 本文件定义了 RpcServer 类（当前为 acceptor 角色）。
 * RpcServer 负责监听客户端连接并把连接分发到 WorkerLoop。
 *
 * ## 架构概述
 *
 * ```
 *                    +------------------------+
 *                    |       RpcServer        |
 *                    |------------------------|
 *                    |  listen_fd_ (socket)   |
 *                    |  acceptor listen fd     |
 *                    |  worker loops            |
 *                    +------------------------+
 *                              |
 *          +-------------------+-------------------+
 *          |                   |                   |
 *    +------------+      +------------+      +------------+
 *    | Connection |      | Connection |      | Connection |
 *    |  (fd=5)    |      |  (fd=6)    |      |  (fd=7)    |
 *    +------------+      +------------+      +------------+
 * ```
 *
 * ## 工作流程
 *
 * 1. **初始化阶段**
 *    - 创建监听 socket
 *    - 设置 SO_REUSEADDR 选项
 *    - 绑定端口并开始监听
 *    - 创建 epoll 实例
 *
 * 2. **接入分发阶段**
 *    - 等待 listen fd 就绪
 *    - accept 新连接
 *    - 分发给某个 WorkerLoop（当前固定单 worker）
 *
 * 3. **Worker 驱动阶段**
 *    - WorkerLoop 负责连接 epoll 驱动、协程推进和连接生命周期
 *
 * ## 使用示例
 *
 * @code
 *   // 创建服务注册表并注册方法
 *   ServiceRegistry registry;
 *   registry.Register("Calculator", "Add", AddHandler);
 *
 *   // 创建并启动服务器
 *   RpcServer server(8080, registry);
 *   if (!server.Start()) {
 *     std::cerr << "Server start failed\n";
 *     return 1;
 *   }
 * @endcode
 *
 * ## 设计特点
 *
 * - **结构先行**：Acceptor 与 WorkerLoop 职责分离
 * - **单 worker 运行**：当前仅创建一个 WorkerLoop
 * - **非阻塞 I/O**：所有 socket 均为非阻塞模式
 * - **RAII 资源管理**：使用 UniqueFd 自动管理文件描述符
 * - **优雅关闭**：连接关闭时自动清理 epoll 注册
 *
 * @see Connection 处理单个客户端连接
 * @see ServiceRegistry 服务方法注册表
 * @author RPC Framework Team
 * @date 2024
 */

#pragma once

#include <cstddef>
#include <cstdint>  // std::uint16_t

#include "common/unique_fd.h"         // RAII 文件描述符封装
#include "server/service_registry.h"  // 服务注册表

namespace rpc::server {

/**
 * @class RpcServer
 * @brief Acceptor + 多 WorkerLoop RPC 服务端
 *
 * RpcServer 负责：
 * - 监听指定端口的客户端连接
 * - accept 新连接
 * - 将连接分发给 WorkerLoop（round-robin）
 *
 * ## 核心职责
 *
 * 1. **连接接入**
 *    - 监听 socket 上的新连接请求
 *    - 使用 accept4 创建非阻塞客户端 socket
 *    - 将新连接注册到 epoll
 *
 * 2. **连接分发**
 *    - 将新连接交给 WorkerLoop 接管
 *
 * 3. **线程边界**
 *    - RpcServer 不直接驱动 Connection
 *    - Connection 归属某个 WorkerLoop
 *
 * ## 线程模型
 *
 * 当前实现为 one-loop-per-thread：
 * - acceptor 线程负责 listen/accept/分发
 * - 每个 WorkerLoop 在独立线程中驱动所属连接
 *
 * @note 服务端目前不支持优雅关闭，调用 Start() 后将无限循环
 */
class RpcServer {
 public:
  /**
   * @brief 构造 RpcServer 实例
   *
   * @param port 监听端口号（主机字节序）
   * @param registry 服务注册表的常量引用
   *
   * @note registry 必须在 RpcServer 整个生命周期内保持有效
   * @note 构造函数不会创建 socket 或绑定端口，实际初始化在 Start() 中进行
   *
   * ## 示例
   *
   * @code
   *   ServiceRegistry registry;
   *   // ... 注册服务方法 ...
   *
   *   RpcServer server(8080, registry);  // 在 8080 端口监听
   * @endcode
   */
  RpcServer(std::uint16_t port, const ServiceRegistry& registry,
            std::size_t worker_count = 2U);

  /**
   * @brief 启动服务端事件循环
   *
   * 执行完整的初始化流程并进入无限事件循环。
   *
   * ## 初始化流程
   *
   * 1. 创建 TCP socket
   * 2. 设置 SO_REUSEADDR 选项
   * 3. 设置非阻塞模式
   * 4. 绑定端口
   * 5. 开始监听（backlog=128）
   * 6. 创建 epoll 实例
   * 7. 注册监听 socket 到 epoll
   *
   * ## 事件循环
   *
   * 进入无限循环处理事件：
   * - 新连接：accept4 创建客户端 socket，注册到 epoll
   * - 读事件：调用 Connection::OnReadable()
   * - 写事件：调用 Connection::OnWritable()
   * - 错误/关闭：清理连接资源
   *
   * @return true 不应该到达（无限循环）
   * @return false 初始化失败或运行时致命错误
   *
   * ## 错误处理
   *
   * 以下情况会返回 false：
   * - socket() 创建失败
   * - setsockopt() 设置失败
   * - fcntl() 设置非阻塞失败
   * - bind() 绑定失败（端口被占用）
   * - listen() 监听失败
   * - epoll_create1() 创建失败
   * - epoll_ctl() 注册失败
   * - epoll_wait() 返回错误（非 EINTR）
   *
   * @note 此方法会阻塞当前线程，直到发生致命错误
   * @note 目前没有提供优雅关闭的机制
   *
   * ## 示例
   *
   * @code
   *   RpcServer server(8080, registry);
   *   if (!server.Start()) {
   *     std::cerr << "Server failed to start\n";
   *     return 1;
   *   }
   *   // 不会到达这里
   * @endcode
   */
  bool Start();

  // ===========================================================================
  // 成员变量
  // ===========================================================================

  /**
   * @brief 监听端口号
   *
   * 服务端绑定的 TCP 端口号，主机字节序。
   * 有效范围：1-65535，建议使用 1024 以上的端口。
   */
  std::uint16_t port_;

  std::size_t worker_count_;

  /**
   * @brief 服务注册表引用
   *
   * 持有 ServiceRegistry 的常量引用，用于查找请求对应的处理函数。
   *
   * @warning 调用者必须确保 registry 在 RpcServer 整个生命周期内有效
   */
  const ServiceRegistry& registry_;
};

}  // namespace rpc::server