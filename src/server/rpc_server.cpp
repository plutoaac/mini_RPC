#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>

#include "common/log.h"
#include "common/rpc_error.h"
#include "common/unique_fd.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace rpc::server {

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port), registry_(registry) {}

bool RpcServer::Start() {
  // 监听 fd 交给 RAII 对象托管，函数退出时自动 close。
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    common::LogError(std::string("socket failed: ") + std::strerror(errno));
    return false;
  }

  int reuse = 1;
  // 重启服务时避免地址占用（TIME_WAIT）导致 bind 失败。
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    common::LogError(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                     std::strerror(errno));
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    common::LogError(std::string("bind failed: ") + std::strerror(errno));
    return false;
  }

  if (::listen(listen_fd.Get(), 128) < 0) {
    common::LogError(std::string("listen failed: ") + std::strerror(errno));
    return false;
  }

  common::LogInfo("RPC server listening on port " + std::to_string(port_));

  while (true) {
    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    // 每个连接 fd 也由 RAII 托管，离开循环体自动释放。
    common::UniqueFd client_fd(
        ::accept(listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
                 &client_addr_len));
    if (!client_fd) {
      if (errno == EINTR) {
        continue;
      }
      common::LogError(std::string("accept failed: ") + std::strerror(errno));
      continue;
    }

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    common::LogInfo(std::string("client connected: ") + ip + ':' +
                    std::to_string(ntohs(client_addr.sin_port)));

    (void)HandleClient(client_fd);
    common::LogInfo("client disconnected");
  }
}

bool RpcServer::HandleClient(const rpc::common::UniqueFd& client_fd) const {
  while (true) {
    rpc::RpcRequest request;
    std::string read_error;
    // 读取并解码 RpcRequest。
    if (!protocol::Codec::ReadMessage(client_fd.Get(), &request, &read_error)) {
      if (read_error != "peer closed connection") {
        common::LogError("read request failed: " + read_error);
      }
      return false;
    }

    rpc::RpcResponse response;
    response.set_request_id(request.request_id());
    response.set_error_code(rpc::OK);

    // 根据 service/method 查找 handler 并执行业务逻辑。
    if (const auto handler =
            registry_.Find(request.service_name(), request.method_name());
        !handler.has_value()) {
      response.set_error_code(rpc::METHOD_NOT_FOUND);
      response.set_error_msg("method not found: " + request.service_name() +
                             "." + request.method_name());
    } else {
      try {
        // 框架层只传递 bytes，不依赖具体业务 protobuf 类型。
        const std::string resp_payload = (*handler)(request.payload());
        response.set_payload(resp_payload);
      } catch (const RpcError& ex) {
        // 业务显式抛出的框架异常，按统一错误码返回。
        response.set_error_code(common::ToProtoErrorCode(ex.code()));
        response.set_error_msg(ex.what());
      } catch (const std::exception& ex) {
        response.set_error_code(common::ToProtoErrorCode(
            common::make_error_code(common::ErrorCode::kInternalError)));
        response.set_error_msg(std::string("handler exception: ") + ex.what());
      } catch (...) {
        response.set_error_code(common::ToProtoErrorCode(
            common::make_error_code(common::ErrorCode::kInternalError)));
        response.set_error_msg("handler threw unknown exception");
      }
    }

    std::string write_error;
    // 编码并写回 RpcResponse。
    if (!protocol::Codec::WriteMessage(client_fd.Get(), response,
                                       &write_error)) {
      common::LogError("write response failed: " + write_error);
      return false;
    }

    common::LogInfo("handled request_id=" + request.request_id() +
                    " service=" + request.service_name() +
                    " method=" + request.method_name() +
                    " code=" + std::to_string(response.error_code()));
  }
}

}  // namespace rpc::server
