/**
 * @file rpc_server.h
 * @brief RPC 服务器核心模块
 *
 * 本文件定义了 RpcServer 类，实现了阻塞式的 RPC 服务器。
 * RpcServer 负责：
 * - 监听指定端口的 TCP 连接
 * - 接受客户端连接
 * - 为每个连接创建 Connection 对象进行处理
 *
 * ## 架构设计
 *
 * ```
 *                    +------------------+
 *                    |   RpcServer      |
 *                    |------------------|
 *    Client -------->|  accept() loop   |
 *                    |        |         |
 *                    |        v         |
 *                    |  Connection      |
 *                    |  (per client)    |
 *                    +------------------+
 * ```
 *
 * ## 特点
 *
 * - **阻塞式设计**：采用同步阻塞 I/O 模型，简单可靠
 * - **串行处理**：一次只处理一个客户端连接
 * - **RAII 资源管理**：通过 UniqueFd 自动管理 socket 生命周期
 *
 * ## 适用场景
 *
 * - 连接数较少的内部服务
 * - 对延迟不敏感的后台任务
 * - 开发测试环境
 *
 * ## 使用示例
 *
 * @code
 *   // 创建服务注册表并注册方法
 *   ServiceRegistry registry;
 *   registry.Register("Calculator", "Add", [](std::string_view req) {
 *     // 处理请求...
 *     return response;
 *   });
 *
 *   // 创建并启动服务器
 *   RpcServer server(8080, registry);
 *   server.Start();  // 阻塞运行
 * @endcode
 *
 * @see Connection 客户端连接处理
 * @see ServiceRegistry 服务方法注册
 * @author RPC Framework Team
 * @date 2024
 */

#pragma once

#include <cstdint>  // std::uint16_t

#include "common/unique_fd.h"         // RAII 文件描述符包装
#include "server/service_registry.h"  // 服务注册表

namespace rpc::server {

/**
 * @class RpcServer
 * @brief 阻塞式 RPC 服务器
 *
 * RpcServer 是一个简单的阻塞式 RPC 服务器实现。
 * 它监听指定端口，接受客户端连接，并为每个连接创建
 * Connection 对象进行处理。
 *
 * ## 服务器生命周期
 *
 * 1. **创建**：指定监听端口和服务注册表
 * 2. **启动**：调用 Start() 开始监听和接受连接
 * 3. **运行**：持续处理连接直到进程终止
 *
 * ## 连接处理模型
 *
 * ```
 * while (true) {
 *   client_fd = accept(listen_fd);
 *   Connection conn(client_fd, registry);
 *   conn.Serve();  // 阻塞处理直到连接关闭
 * }
 * ```
 *
 * ## 限制
 *
 * - 一次只能处理一个客户端连接
 * - 不支持并发请求
 * - 不支持优雅关闭
 *
 * @note 此服务器设计用于简单场景，生产环境建议使用更高级的服务器
 */
class RpcServer {
 public:
  /**
   * @brief 构造函数
   *
   * 初始化 RPC 服务器配置。
   *
   * @param port 监听端口号（1-65535）
   * @param registry 服务注册表的引用，用于处理 RPC 请求
   *
   * @note registry 必须在 RpcServer 整个生命周期内保持有效
   * @note 构造函数不会绑定端口，实际监听在 Start() 中进行
   *
   * @code
   *   ServiceRegistry registry;
   *   RpcServer server(8080, registry);
   * @endcode
   */
  RpcServer(std::uint16_t port, const ServiceRegistry& registry);

  /**
   * @brief 启动服务器
   *
   * 执行以下操作：
   * 1. 创建 TCP socket
   * 2. 设置 SO_REUSEADDR 选项
   * 3. 绑定到指定端口
   * 4. 开始监听
   * 5. 进入 accept 循环
   *
   * @return true 服务器正常启动（通常不会返回，除非启动失败）
   * @return false 服务器启动失败（如端口被占用）
   *
   * @note 此方法是阻塞的，会无限循环接受连接
   * @note 启动失败时会记录错误日志
   * @note 每个连接处理完成后才会接受下一个连接
   *
   * ## 错误处理
   *
   * - socket 创建失败：返回 false
   * - setsockopt 失败：返回 false
   * - bind 失败：返回 false（可能是端口被占用）
   * - listen 失败：返回 false
   * - accept 失败：记录日志并继续等待新连接
   */
  bool Start();

  // ===========================================================================
  // 成员变量
  // ===========================================================================

  /**
   * @brief 监听端口号
   *
   * 服务器绑定的 TCP 端口号。
   * 范围：1-65535（实际使用时应避免系统保留端口）
   */
  std::uint16_t port_;

  /**
   * @brief 服务注册表引用
   *
   * 用于查找和调用 RPC 方法。
   * 生命周期由外部管理，必须在 RpcServer 整个生命周期内保持有效。
   */
  const ServiceRegistry& registry_;
};

}  // namespace rpc::server