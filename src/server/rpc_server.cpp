#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "common/rpc_error.h"
#include "common/unique_fd.h"
#include "protocol/codec.h"
#include "rpc.pb.h"

namespace rpc::server {

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port), registry_(registry) {}

bool RpcServer::Start() {
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    std::cerr << "[ERROR] socket failed: " << std::strerror(errno) << '\n';
    return false;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    std::cerr << "[ERROR] setsockopt(SO_REUSEADDR) failed: "
              << std::strerror(errno) << '\n';
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    std::cerr << "[ERROR] bind failed: " << std::strerror(errno) << '\n';
    return false;
  }

  if (::listen(listen_fd.Get(), 128) < 0) {
    std::cerr << "[ERROR] listen failed: " << std::strerror(errno) << '\n';
    return false;
  }

  std::cout << "[INFO] RPC server listening on port " << port_ << '\n';

  while (true) {
    sockaddr_in client_addr{};
    socklen_t client_addr_len = sizeof(client_addr);
    common::UniqueFd client_fd(
        ::accept(listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
                 &client_addr_len));
    if (!client_fd) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "[ERROR] accept failed: " << std::strerror(errno) << '\n';
      continue;
    }

    char ip[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    std::cout << "[INFO] client connected: " << ip << ':'
              << ntohs(client_addr.sin_port) << '\n';

    (void)HandleClient(client_fd.Get());
    std::cout << "[INFO] client disconnected\n";
  }
}

bool RpcServer::HandleClient(int client_fd) const {
  while (true) {
    rpc::RpcRequest request;
    std::string read_error;
    if (!protocol::Codec::ReadMessage(client_fd, &request, &read_error)) {
      if (read_error != "peer closed connection") {
        std::cerr << "[ERROR] read request failed: " << read_error << '\n';
      }
      return false;
    }

    rpc::RpcResponse response;
    response.set_request_id(request.request_id());
    response.set_error_code(rpc::OK);

    if (const auto handler =
            registry_.Find(request.service_name(), request.method_name());
        !handler.has_value()) {
      response.set_error_code(rpc::METHOD_NOT_FOUND);
      response.set_error_msg("method not found: " + request.service_name() +
                             "." + request.method_name());
    } else {
      try {
        const std::string resp_payload = (*handler)(request.payload());
        response.set_payload(resp_payload);
      } catch (const RpcError& ex) {
        response.set_error_code(common::ToProtoErrorCode(ex.code()));
        response.set_error_msg(ex.what());
      } catch (const std::exception& ex) {
        response.set_error_code(rpc::INTERNAL_ERROR);
        response.set_error_msg(std::string("handler exception: ") + ex.what());
      } catch (...) {
        response.set_error_code(rpc::INTERNAL_ERROR);
        response.set_error_msg("handler threw unknown exception");
      }
    }

    std::string write_error;
    if (!protocol::Codec::WriteMessage(client_fd, response, &write_error)) {
      std::cerr << "[ERROR] write response failed: " << write_error << '\n';
      return false;
    }

    std::cout << "[INFO] handled request_id=" << request.request_id()
              << " service=" << request.service_name()
              << " method=" << request.method_name()
              << " code=" << response.error_code() << '\n';
  }
}

}  // namespace rpc::server
