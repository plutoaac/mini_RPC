#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace google::protobuf {
class Message;
}

namespace rpc::protocol {

// 协议编解码器：实现传输层协议
// [4字节大端长度][protobuf序列化数据]。
class Codec {
 public:
  // 从阻塞 socket 读取一个完整 protobuf 消息。
  // 成功返回 true；失败返回 false，并通过 error_msg 给出原因。
  static bool ReadMessage(int fd, google::protobuf::Message* message,
                          std::string* error_msg);

  // 将 protobuf 消息序列化后写入阻塞 socket。
  // 成功返回 true；失败返回 false，并通过 error_msg 给出原因。
  static bool WriteMessage(int fd, const google::protobuf::Message& message,
                           std::string* error_msg);

 private:
  // 限制单帧上限，避免异常包导致的内存放大。
  static constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

  // 保证恰好读取 N 字节。
  static bool ReadN(int fd, std::span<std::byte> buffer,
                    std::string* error_msg);
  // 保证恰好写入 N 字节。
  static bool WriteN(int fd, std::span<const std::byte> buffer,
                     std::string* error_msg);
};

}  // namespace rpc::protocol
