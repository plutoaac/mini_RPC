#include "protocol/codec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <google/protobuf/message.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace rpc::protocol {

bool Codec::ReadN(int fd, std::span<std::byte> buffer, std::string* error_msg) {
  // 将 span 视图转换成字节指针，循环读取直到填满 buffer。
  auto* out = reinterpret_cast<std::uint8_t*>(buffer.data());
  std::size_t left = buffer.size_bytes();

  while (left > 0) {
    const ssize_t rc = ::recv(fd, out, left, 0);
    if (rc == 0) {
      // 对端正常关闭连接。
      if (error_msg != nullptr) {
        *error_msg = "peer closed connection";
      }
      return false;
    }
    if (rc < 0) {
      // 被信号中断时继续读，其他错误直接返回。
      if (errno == EINTR) {
        continue;
      }
      if (error_msg != nullptr) {
        *error_msg = std::string("recv failed: ") + std::strerror(errno);
      }
      return false;
    }
    out += static_cast<std::size_t>(rc);
    left -= static_cast<std::size_t>(rc);
  }

  return true;
}

bool Codec::WriteN(int fd, std::span<const std::byte> buffer,
                   std::string* error_msg) {
  // 循环发送，确保完整发送整个 buffer。
  const auto* in = reinterpret_cast<const std::uint8_t*>(buffer.data());
  std::size_t left = buffer.size_bytes();

  while (left > 0) {
    const ssize_t rc = ::send(fd, in, left, MSG_NOSIGNAL);
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (error_msg != nullptr) {
        *error_msg = std::string("send failed: ") + std::strerror(errno);
      }
      return false;
    }
    in += static_cast<std::size_t>(rc);
    left -= static_cast<std::size_t>(rc);
  }

  return true;
}

bool Codec::ReadMessage(int fd, google::protobuf::Message* message,
                        std::string* error_msg) {
  if (message == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "message is null";
    }
    return false;
  }

  std::uint32_t be_length = 0;
  // 先读固定 4 字节帧头（大端长度）。
  auto length_bytes =
      std::as_writable_bytes(std::span{&be_length, std::size_t{1}});
  if (!ReadN(fd, length_bytes, error_msg)) {
    return false;
  }

  const std::uint32_t body_length = ntohl(be_length);
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid frame length: " + std::to_string(body_length);
    }
    return false;
  }

  std::vector<char> buffer(body_length);
  // 再读完整消息体。
  auto body_bytes = std::as_writable_bytes(std::span{buffer});
  if (!ReadN(fd, body_bytes, error_msg)) {
    return false;
  }

  if (!message->ParseFromArray(buffer.data(),
                               static_cast<int>(buffer.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to parse protobuf body";
    }
    return false;
  }

  return true;
}

bool Codec::WriteMessage(int fd, const google::protobuf::Message& message,
                         std::string* error_msg) {
  const std::size_t body_length =
      static_cast<std::size_t>(message.ByteSizeLong());
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid serialized size: " + std::to_string(body_length);
    }
    return false;
  }

  std::string body;
  body.resize(body_length);
  // 先将 protobuf 序列化到连续内存。
  if (!message.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize protobuf message";
    }
    return false;
  }

  const std::uint32_t be_length =
      htonl(static_cast<std::uint32_t>(body_length));
  // 按 [长度][消息体] 顺序发送。
  const auto length_bytes =
      std::as_bytes(std::span{&be_length, std::size_t{1}});
  if (!WriteN(fd, length_bytes, error_msg)) {
    return false;
  }
  const auto body_bytes = std::as_bytes(std::span{body.data(), body.size()});
  if (!WriteN(fd, body_bytes, error_msg)) {
    return false;
  }

  return true;
}

}  // namespace rpc::protocol
