#include "protocol/codec.h"

#include <arpa/inet.h>
#include <errno.h>
#include <google/protobuf/message.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace rpc::protocol {

bool Codec::ReadN(int fd, void* buffer, std::size_t n, std::string* error_msg) {
  auto* out = static_cast<std::uint8_t*>(buffer);
  std::size_t left = n;

  while (left > 0) {
    const ssize_t rc = ::recv(fd, out, left, 0);
    if (rc == 0) {
      if (error_msg != nullptr) {
        *error_msg = "peer closed connection";
      }
      return false;
    }
    if (rc < 0) {
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

bool Codec::WriteN(int fd, const void* buffer, std::size_t n,
                   std::string* error_msg) {
  const auto* in = static_cast<const std::uint8_t*>(buffer);
  std::size_t left = n;

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
  if (!ReadN(fd, &be_length, sizeof(be_length), error_msg)) {
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
  if (!ReadN(fd, buffer.data(), body_length, error_msg)) {
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
  if (!message.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize protobuf message";
    }
    return false;
  }

  const std::uint32_t be_length =
      htonl(static_cast<std::uint32_t>(body_length));
  if (!WriteN(fd, &be_length, sizeof(be_length), error_msg)) {
    return false;
  }
  if (!WriteN(fd, body.data(), body.size(), error_msg)) {
    return false;
  }

  return true;
}

}  // namespace rpc::protocol
