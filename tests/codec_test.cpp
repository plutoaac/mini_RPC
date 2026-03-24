#include "protocol/codec.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "rpc.pb.h"

namespace {

void TestNormalEncodeDecode() {
  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  rpc::RpcRequest request;
  request.set_request_id("1");
  request.set_service_name("CalcService");
  request.set_method_name("Add");
  request.set_payload("hello");

  std::string error;
  const bool write_ok =
      rpc::protocol::Codec::WriteMessage(fds[0], request, &error);
  assert(write_ok);

  rpc::RpcRequest decoded;
  const bool read_ok =
      rpc::protocol::Codec::ReadMessage(fds[1], &decoded, &error);
  assert(read_ok);
  assert(decoded.request_id() == "1");
  assert(decoded.service_name() == "CalcService");
  assert(decoded.method_name() == "Add");
  assert(decoded.payload() == "hello");

  ::close(fds[0]);
  ::close(fds[1]);
}

void TestZeroLengthFrameError() {
  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  const std::uint32_t zero = htonl(0);
  assert(::write(fds[0], &zero, sizeof(zero)) ==
         static_cast<ssize_t>(sizeof(zero)));

  rpc::RpcRequest decoded;
  std::string error;
  const bool ok = rpc::protocol::Codec::ReadMessage(fds[1], &decoded, &error);
  assert(!ok);
  assert(error.find("invalid frame length") != std::string::npos);

  ::close(fds[0]);
  ::close(fds[1]);
}

void TestTooLargeFrameError() {
  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  constexpr std::uint32_t kTooLarge = 4 * 1024 * 1024 + 1;
  const std::uint32_t be_len = htonl(kTooLarge);
  assert(::write(fds[0], &be_len, sizeof(be_len)) ==
         static_cast<ssize_t>(sizeof(be_len)));

  rpc::RpcRequest decoded;
  std::string error;
  const bool ok = rpc::protocol::Codec::ReadMessage(fds[1], &decoded, &error);
  assert(!ok);
  assert(error.find("invalid frame length") != std::string::npos);

  ::close(fds[0]);
  ::close(fds[1]);
}

void TestProtobufParseError() {
  int fds[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  // 发送一个非法 protobuf（截断的 varint），触发解析失败。
  const std::vector<unsigned char> body = {0x80};
  const std::uint32_t be_len = htonl(static_cast<std::uint32_t>(body.size()));
  assert(::write(fds[0], &be_len, sizeof(be_len)) ==
         static_cast<ssize_t>(sizeof(be_len)));
  assert(::write(fds[0], body.data(), body.size()) ==
         static_cast<ssize_t>(body.size()));

  rpc::RpcRequest decoded;
  std::string error;
  const bool ok = rpc::protocol::Codec::ReadMessage(fds[1], &decoded, &error);
  assert(!ok);
  assert(error.find("failed to parse protobuf body") != std::string::npos);

  ::close(fds[0]);
  ::close(fds[1]);
}

}  // namespace

int main() {
  TestNormalEncodeDecode();
  TestZeroLengthFrameError();
  TestTooLargeFrameError();
  TestProtobufParseError();

  std::cout << "codec_test passed\n";
  return 0;
}
