#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace google::protobuf {
class Message;
}

namespace rpc::protocol {

// Codec implements the transport protocol:
// [4-byte big-endian length][protobuf serialized bytes].
class Codec {
 public:
  // Read one full protobuf message from a blocking TCP socket.
  // Returns true on success; false on EOF or error, with details in error_msg.
  static bool ReadMessage(int fd, google::protobuf::Message* message,
                          std::string* error_msg);

  // Serialize and write one protobuf message to a blocking TCP socket.
  // Returns true on success; false on error, with details in error_msg.
  static bool WriteMessage(int fd, const google::protobuf::Message& message,
                           std::string* error_msg);

 private:
  static constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

  static bool ReadN(int fd, std::span<std::byte> buffer,
                    std::string* error_msg);
  static bool WriteN(int fd, std::span<const std::byte> buffer,
                     std::string* error_msg);
};

}  // namespace rpc::protocol
