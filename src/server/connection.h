/**
 * @file connection.h
 * @brief RPC 服务器连接处理模块
 *
 * 本文件定义了 Connection 类，负责处理单个客户端连接的完整生命周期。
 * Connection 类实现了 RPC 协议的解析、请求分发和响应发送功能。
 *
 * 主要职责：
 * - 管理 socket 文件描述符的生命周期（通过 UniqueFd RAII）
 * - 维护读写缓冲区，实现协议帧的解析与组装
 * - 调用 ServiceRegistry 进行请求方法的路由与执行
 *
 * 协议格式：
 * - 帧头：4 字节大端序长度字段，表示消息体长度
 * - 帧体：Protobuf 序列化的 RpcRequest 或 RpcResponse
 *
 * @author RPC Framework Team
 * @date 2024
 */

#pragma once

#include <cstddef>
#include <string>

#include "common/unique_fd.h"

namespace rpc {
class RpcRequest;   // Protobuf 生成的请求消息类
class RpcResponse;  // Protobuf 生成的响应消息类
}  // namespace rpc

namespace rpc::server {

class ServiceRegistry;  // 前向声明：服务注册表，用于方法查找

/**
 * @class Connection
 * @brief 连接级处理对象，维护单个客户端连接的完整生命周期
 *
 * Connection 类是 RPC 服务器处理客户端请求的核心组件。每个客户端连接
 * 对应一个 Connection 实例，负责：
 *
 * 1. **数据读取**：从 socket 读取数据到内部缓冲区
 * 2. **协议解析**：按照自定义帧协议解析请求消息
 * 3. **请求处理**：通过 ServiceRegistry 查找并调用对应的方法处理器
 * 4. **响应发送**：将处理结果序列化并写回客户端
 *
 * ## 设计特点
 *
 * - **阻塞式 I/O**：采用同步阻塞模型，适用于连接数较少的场景
 * - **RAII 资源管理**：通过 UniqueFd 自动管理 socket 生命周期
 * - **缓冲区复用**：读写缓冲区在整个连接生命周期内复用
 *
 * ## 线程安全
 *
 * Connection 实例不具备线程安全性，不应在多线程间共享。
 * 每个 Connection 应在单一线程中独占使用。
 *
 * ## 使用示例
 *
 * @code
 *   // 创建连接实例
 *   Connection conn(std::move(client_fd), registry);
 *
 *   // 启动服务循环（阻塞直到连接关闭）
 *   bool normal_close = conn.Serve();
 * @endcode
 *
 * @see ServiceRegistry 方法注册与查找
 * @see UniqueFd 文件描述符 RAII 包装
 */
class Connection {
 public:
  /**
   * @brief 构造函数，初始化连接实例
   *
   * 接收已建立的客户端 socket 并准备处理请求。
   * 内部会预分配读写缓冲区以提高后续操作效率。
   *
   * @param fd 客户端 socket 文件描述符（通过移动语义转移所有权）
   * @param registry 服务注册表的常量引用，用于查找请求处理方法
   *
   * @note fd 参数通过移动语义传递，调用后原 UniqueFd 对象将处于空状态
   * @note registry 必须在 Connection 整个生命周期内保持有效
   */
  Connection(rpc::common::UniqueFd fd, const ServiceRegistry& registry);

  /**
   * @brief 驱动连接服务循环，处理所有请求直到连接关闭
   *
   * 这是 Connection 的主入口方法。它持续执行以下循环：
   * 1. 从 socket 读取数据
   * 2. 解析完整的请求帧
   * 3. 调用对应的处理方法
   * 4. 发送响应数据
   *
   * 循环终止条件：
   * - 客户端主动关闭连接（返回 true）
   * - 发生不可恢复的错误（返回 false）
   *
   * @return true 表示连接正常结束（对端关闭或 EOF）
   * @return false 表示处理过程中出现错误
   *
   * @note 此方法是阻塞的，会一直运行直到连接关闭
   * @note 返回后，连接对象不再可用，应被销毁
   */
  [[nodiscard]] bool Serve();

 private:
  /**
   * @enum ReadResult
   * @brief 从 socket 读取操作的结果状态
   *
   * 用于区分不同的读取结果，以便调用者做出相应处理。
   */
  enum class ReadResult {
    kData,        ///< 成功读取到数据
    kPeerClosed,  ///< 对端已关闭连接（收到 FIN）
    kError,       ///< 读取过程中发生错误
  };

  /**
   * @brief 从 socket 读取数据到内部缓冲区
   *
   * 执行非阻塞读取操作，将接收到的数据追加到 read_buffer_。
   *
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return ReadResult 读取结果状态
   *
   * @note 内部处理 EINTR 信号中断，会自动重试
   * @note 每次调用最多读取 4096 字节数据
   */
  [[nodiscard]] ReadResult ReadFromSocket(std::string* error_msg);

  /**
   * @brief 尝试从缓冲区解析并处理所有完整的请求帧
   *
   * 解析 read_buffer_ 中的数据，提取所有完整的请求帧并逐个处理。
   * 已处理的数据会从缓冲区移除。
   *
   * 处理流程：
   * 1. 检查是否有足够的字节读取帧头
   * 2. 解析帧头获取消息体长度
   * 3. 检查是否已接收完整的消息体
   * 4. 反序列化 RpcRequest 并处理
   * 5. 将响应写入发送缓冲区
   *
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return true 解析和处理成功（即使解析失败也会生成错误响应）
   * @return false 发生不可恢复的错误（如响应队列写入失败）
   *
   * @note 即使请求解析失败，也会生成错误响应而非返回 false
   */
  [[nodiscard]] bool TryParseRequests(std::string* error_msg);

  /**
   * @brief 处理单个 RPC 请求并生成响应
   *
   * 根据请求中的服务名和方法名查找注册的处理函数并执行。
   * 处理结果或错误信息会填充到 response 中。
   *
   * 处理逻辑：
   * 1. 设置响应的 request_id（用于客户端匹配）
   * 2. 查找服务方法
   * 3. 执行处理函数
   * 4. 捕获并处理异常（RpcError、std::exception、未知异常）
   *
   * @param request 解析后的 RPC 请求对象
   * @param[out] response 填充处理结果的响应对象
   * @return true 处理完成（包括处理失败但已生成错误响应的情况）
   * @return false 响应指针为空（内部错误）
   *
   * @note 所有异常都会被捕获并转换为错误响应
   */
  [[nodiscard]] bool HandleOneRequest(const rpc::RpcRequest& request,
                                      rpc::RpcResponse* response) const;

  /**
   * @brief 将响应消息加入发送队列
   *
   * 将 RpcResponse 序列化并添加帧头，追加到 write_buffer_。
   *
   * @param response 要发送的响应对象
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return true 成功加入发送队列
   * @return false 序列化失败或帧大小非法
   */
  [[nodiscard]] bool QueueResponse(const rpc::RpcResponse& response,
                                   std::string* error_msg);

  /**
   * @brief 将发送缓冲区中的数据写入 socket
   *
   * 将 write_buffer_ 中的所有数据发送到客户端。
   * 成功发送的数据会从缓冲区移除。
   *
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return true 所有数据发送完成
   * @return false 发送过程中发生错误
   *
   * @note 使用 MSG_NOSIGNAL 防止对端关闭时产生 SIGPIPE 信号
   * @note 内部处理 EINTR 信号中断，会自动重试
   */
  [[nodiscard]] bool FlushWrites(std::string* error_msg);

  /**
   * @brief 编码协议帧
   *
   * 将消息体封装为完整的协议帧（帧头 + 帧体）。
   *
   * 帧格式：
   * +----------------+------------------+
   * | 4 bytes header | N bytes body     |
   * | (big-endian)   | (protobuf data)  |
   * +----------------+------------------+
   *
   * @param body 消息体数据
   * @param[out] frame 输出完整帧数据
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return true 编码成功
   * @return false 参数非法或帧大小超出限制
   */
  [[nodiscard]] static bool EncodeFrame(const std::string& body,
                                        std::string* frame,
                                        std::string* error_msg);

  /**
   * @brief 解码协议帧头
   *
   * 从缓冲区指定位置读取并解析帧头，获取消息体长度。
   *
   * @param buffer 包含帧数据的缓冲区
   * @param offset 帧头在缓冲区中的起始偏移量
   * @param[out] body_length 输出消息体长度
   * @param[out] error_msg 如果发生错误，输出错误描述信息
   * @return true 解码成功
   * @return false 参数非法或帧长度值非法（0 或超过最大限制）
   */
  [[nodiscard]] static bool DecodeFrameHeader(const std::string& buffer,
                                              std::size_t offset,
                                              std::size_t* body_length,
                                              std::string* error_msg);

  // ============================================================================
  // 成员变量
  // ============================================================================

  /**
   * @brief 客户端 socket 文件描述符
   *
   * 通过 UniqueFd RAII 包装，确保连接销毁时自动关闭。
   */
  rpc::common::UniqueFd fd_;

  /**
   * @brief 服务注册表引用
   *
   * 用于查找请求对应的处理方法。生命周期由外部管理，
   * 必须在 Connection 整个生命周期内保持有效。
   */
  const ServiceRegistry& registry_;

  /**
   * @brief 接收缓冲区
   *
   * 存储从 socket 读取的原始数据，等待解析。
   * 构造时预分配 8KB 空间以减少扩容开销。
   */
  std::string read_buffer_;

  /**
   * @brief 发送缓冲区
   *
   * 存储待发送的响应数据（已封装为协议帧）。
   * 构造时预分配 8KB 空间以减少扩容开销。
   */
  std::string write_buffer_;
};

}  // namespace rpc::server