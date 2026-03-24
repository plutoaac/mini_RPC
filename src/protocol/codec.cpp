#include "protocol/codec.h"

// 网络编程相关头文件
#include <arpa/inet.h>   // htonl, ntohl 等字节序转换函数
#include <errno.h>       // errno 错误码
#include <google/protobuf/message.h>  // protobuf 消息基类
#include <sys/socket.h>  // send, recv 等 socket API
#include <unistd.h>      // POSIX API

#include <cstddef>   // std::size_t, std::byte
#include <cstring>   // std::strerror
#include <span>      // std::span 连续内存视图
#include <string>    // std::string
#include <vector>    // std::vector 动态数组

namespace rpc::protocol {

/// 精确读取 N 字节
///
/// 核心思路：TCP 是流式协议，单次 recv 返回的数据可能少于请求量。
/// 此函数通过循环调用 recv，直到填满指定长度的缓冲区。
///
/// @param fd socket 文件描述符
/// @param buffer 目标缓冲区，使用 std::span 避免额外的内存分配
/// @param error_msg 错误信息输出
/// @return 成功返回 true，失败返回 false
bool Codec::ReadN(int fd, std::span<std::byte> buffer, std::string* error_msg) {
  // 将 span 视图转换成字节指针，便于指针算术操作
  auto* out = reinterpret_cast<std::uint8_t*>(buffer.data());
  std::size_t left = buffer.size_bytes();  // 剩余待读取字节数

  // 循环读取直到填满缓冲区
  while (left > 0) {
    // ::recv 确保调用全局命名空间的系统函数
    // 参数：socket fd, 缓冲区地址, 期望读取字节数, 标志位(0表示阻塞)
    const ssize_t rc = ::recv(fd, out, left, 0);

    if (rc == 0) {
      // 返回 0 表示对端已关闭连接（EOF）
      // 这是正常的连接关闭方式，不是错误
      if (error_msg != nullptr) {
        *error_msg = "peer closed connection";
      }
      return false;
    }
    if (rc < 0) {
      // 返回负值表示出错
      // EINTR: 系统调用被信号中断，可以安全重试
      if (errno == EINTR) {
        continue;  // 被信号中断，重新尝试读取
      }
      // 其他错误（如超时 ETIMEDOUT、连接重置 ECONNRESET 等）
      if (error_msg != nullptr) {
        *error_msg = std::string("recv failed: ") + std::strerror(errno);
      }
      return false;
    }

    // 更新指针和剩余计数，准备下一次读取
    out += static_cast<std::size_t>(rc);   // 移动缓冲区指针
    left -= static_cast<std::size_t>(rc);  // 减少剩余字节数
  }

  return true;
}

/// 精确写入 N 字节
///
/// 核心思路：TCP 是流式协议，单次 send 可能只发送部分数据。
/// 此函数通过循环调用 send，直到发送完指定长度的数据。
///
/// @param fd socket 文件描述符
/// @param buffer 待发送的数据，使用 std::span 避免额外的内存分配
/// @param error_msg 错误信息输出
/// @return 成功返回 true，失败返回 false
bool Codec::WriteN(int fd, std::span<const std::byte> buffer,
                   std::string* error_msg) {
  // 将 span 视图转换成字节指针，便于指针算术操作
  const auto* in = reinterpret_cast<const std::uint8_t*>(buffer.data());
  std::size_t left = buffer.size_bytes();  // 剩余待发送字节数

  // 循环发送直到全部发送完毕
  while (left > 0) {
    // ::send 确保调用全局命名空间的系统函数
    // MSG_NOSIGNAL: 防止对端关闭连接时产生 SIGPIPE 信号导致进程退出
    //   - 如果不使用此标志，向已关闭的 socket 写入会触发 SIGPIPE
    //   - 使用此标志后，send 会返回 -1 并设置 errno 为 EPIPE
    const ssize_t rc = ::send(fd, in, left, MSG_NOSIGNAL);

    if (rc < 0) {
      // 返回负值表示出错
      // EINTR: 系统调用被信号中断，可以安全重试
      if (errno == EINTR) {
        continue;  // 被信号中断，重新尝试发送
      }
      // 其他错误（如连接断开 EPIPE、超时 ETIMEDOUT 等）
      if (error_msg != nullptr) {
        *error_msg = std::string("send failed: ") + std::strerror(errno);
      }
      return false;
    }

    // 更新指针和剩余计数，准备下一次发送
    in += static_cast<std::size_t>(rc);   // 移动缓冲区指针
    left -= static_cast<std::size_t>(rc);  // 减少剩余字节数
  }

  return true;
}

/// 从 socket 读取一个完整的 protobuf 消息
///
/// 协议格式：[4字节大端长度][protobuf序列化数据]
///
/// 读取流程：
/// 1. 读取 4 字节长度头（大端序）
/// 2. 转换为主机字节序，得到消息体长度
/// 3. 校验长度合法性（防止恶意大包）
/// 4. 读取完整消息体
/// 5. 反序列化为 protobuf 消息对象
///
/// @param fd socket 文件描述符
/// @param message 输出参数，反序列化后的 protobuf 消息
/// @param error_msg 错误信息输出
/// @return 成功返回 true，失败返回 false
bool Codec::ReadMessage(int fd, google::protobuf::Message* message,
                        std::string* error_msg) {
  // 参数校验：message 指针不能为空
  if (message == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "message is null";
    }
    return false;
  }

  // ========== 第一步：读取 4 字节长度头 ==========
  std::uint32_t be_length = 0;  // 大端序的长度值

  // std::as_writable_bytes 将变量转换为可写的字节视图
  // 这样可以直接向 be_length 的内存写入原始字节
  auto length_bytes =
      std::as_writable_bytes(std::span{&be_length, std::size_t{1}});

  if (!ReadN(fd, length_bytes, error_msg)) {
    return false;  // 读取长度头失败
  }

  // ========== 第二步：字节序转换 ==========
  // ntohl: network to host long，将大端序转换为主机字节序
  // 大端序（网络字节序）：高位字节在前（地址低）
  // 小端序（x86/ARM 主机）：低位字节在前（地址低）
  const std::uint32_t body_length = ntohl(be_length);

  // ========== 第三步：长度合法性校验 ==========
  // 长度为 0 或超过最大帧大小，视为非法
  // 这是防止恶意包导致内存耗尽的安全措施
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid frame length: " + std::to_string(body_length);
    }
    return false;
  }

  // ========== 第四步：读取消息体 ==========
  // 分配精确大小的缓冲区
  std::vector<char> buffer(body_length);

  // 将 vector 转换为字节视图进行读取
  auto body_bytes = std::as_writable_bytes(std::span{buffer});
  if (!ReadN(fd, body_bytes, error_msg)) {
    return false;  // 读取消息体失败
  }

  // ========== 第五步：protobuf 反序列化 ==========
  // 将二进制数据解析为 protobuf 消息对象
  if (!message->ParseFromArray(buffer.data(),
                               static_cast<int>(buffer.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to parse protobuf body";
    }
    return false;
  }

  return true;
}

/// 将 protobuf 消息序列化后写入 socket
///
/// 协议格式：[4字节大端长度][protobuf序列化数据]
///
/// 写入流程：
/// 1. 计算 protobuf 序列化后的字节长度
/// 2. 校验长度合法性（防止超大消息）
/// 3. 序列化 protobuf 到连续内存
/// 4. 将长度转换为大端序，作为帧头发送
/// 5. 发送消息体
///
/// @param fd socket 文件描述符
/// @param message 待发送的 protobuf 消息
/// @param error_msg 错误信息输出
/// @return 成功返回 true，失败返回 false
bool Codec::WriteMessage(int fd, const google::protobuf::Message& message,
                         std::string* error_msg) {
  // ========== 第一步：计算序列化大小 ==========
  // ByteSizeLong() 返回 protobuf 序列化后的字节长度
  const std::size_t body_length =
      static_cast<std::size_t>(message.ByteSizeLong());

  // ========== 第二步：长度合法性校验 ==========
  // 空消息或超大消息都是非法的
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid serialized size: " + std::to_string(body_length);
    }
    return false;
  }

  // ========== 第三步：序列化到内存 ==========
  // 分配并填充序列化后的二进制数据
  std::string body;
  body.resize(body_length);

  // SerializeToArray 将消息序列化到预分配的内存
  // 比 SerializeToString 更高效，避免额外的内存分配
  if (!message.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize protobuf message";
    }
    return false;
  }

  // ========== 第四步：发送长度头 ==========
  // htonl: host to network long，将主机字节序转换为大端序
  // 确保不同字节序的主机之间能正确通信
  const std::uint32_t be_length =
      htonl(static_cast<std::uint32_t>(body_length));

  // std::as_bytes 将变量转换为只读字节视图
  // 用于发送长度头的二进制表示
  const auto length_bytes =
      std::as_bytes(std::span{&be_length, std::size_t{1}});

  if (!WriteN(fd, length_bytes, error_msg)) {
    return false;  // 发送长度头失败
  }

  // ========== 第五步：发送消息体 ==========
  // 将序列化后的 protobuf 数据发送出去
  const auto body_bytes = std::as_bytes(std::span{body.data(), body.size()});
  if (!WriteN(fd, body_bytes, error_msg)) {
    return false;  // 发送消息体失败
  }

  return true;
}

}  // namespace rpc::protocol
