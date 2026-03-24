#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

// 前向声明 protobuf Message 类，避免在头文件中引入 protobuf 重型依赖
namespace google::protobuf {
class Message;
}

namespace rpc::protocol {

/// 协议编解码器
///
/// 实现自定义传输层协议，帧格式为：
/// +----------------+------------------------+
/// | 4 字节长度头   | protobuf 序列化数据    |
/// | (大端序)       | (变长)                 |
/// +----------------+------------------------+
///
/// 这种设计的好处：
/// - 长度前缀使接收方能准确知道消息边界，解决 TCP 粘包问题
/// - 大端序是网络字节序，保证不同平台间的兼容性
/// - protobuf 提供高效的结构化序列化
///
/// 使用示例：
/// @code
/// rpc::RpcRequest request;
/// request.set_service_name("Calculator");
/// if (!Codec::WriteMessage(fd, request, &error)) {
///   // 处理错误
/// }
/// @endcode
class Codec {
 public:
  /// 从阻塞式 socket 读取一个完整的 protobuf 消息
  ///
  /// 内部流程：
  /// 1. 先读取 4 字节长度头（大端序）
  /// 2. 根据 length 分配缓冲区
  /// 3. 读取完整消息体
  /// 4. 反序列化为 protobuf 消息
  ///
  /// @param fd socket 文件描述符
  /// @param message 输出参数，反序列化后的 protobuf 消息
  /// @param error_msg 输出参数，失败时的错误信息（可为 nullptr）
  /// @return 成功返回 true，失败返回 false
  static bool ReadMessage(int fd, google::protobuf::Message* message,
                          std::string* error_msg);

  /// 将 protobuf 消息序列化后写入阻塞式 socket
  ///
  /// 内部流程：
  /// 1. 序列化 protobuf 消息到字节流
  /// 2. 写入 4 字节长度头（大端序）
  /// 3. 写入消息体
  ///
  /// @param fd socket 文件描述符
  /// @param message 待发送的 protobuf 消息
  /// @param error_msg 输出参数，失败时的错误信息（可为 nullptr）
  /// @return 成功返回 true，失败返回 false
  static bool WriteMessage(int fd, const google::protobuf::Message& message,
                           std::string* error_msg);

 private:
  /// 单帧最大大小限制（4MB）
  /// 防止恶意或异常的大包导致内存耗尽
  static constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

  /// 精确读取 N 字节
  ///
  /// 由于 TCP 是流式协议，单次 recv 可能返回部分数据。
  /// 此函数循环调用 recv 直到填满指定长度的缓冲区。
  ///
  /// @param fd socket 文件描述符
  /// @param buffer 目标缓冲区
  /// @param error_msg 输出参数，失败时的错误信息（可为 nullptr）
  /// @return 成功返回 true，失败返回 false
  static bool ReadN(int fd, std::span<std::byte> buffer,
                    std::string* error_msg);

  /// 精确写入 N 字节
  ///
  /// 由于 TCP 是流式协议，单次 send 可能只发送部分数据。
  /// 此函数循环调用 send 直到发送完指定长度的数据。
  ///
  /// @param fd socket 文件描述符
  /// @param buffer 待发送的数据
  /// @param error_msg 输出参数，失败时的错误信息（可为 nullptr）
  /// @return 成功返回 true，失败返回 false
  static bool WriteN(int fd, std::span<const std::byte> buffer,
                     std::string* error_msg);
};

}  // namespace rpc::protocol
