#pragma once

#include <cstdint>

#include "server/service_registry.h"

namespace rpc::server {

class RpcServer {
 public:
  RpcServer(std::uint16_t port, const ServiceRegistry& registry);

  // Start accept loop (blocking).
  bool Start();

 private:
  bool HandleClient(int client_fd) const;

  std::uint16_t port_;
  const ServiceRegistry& registry_;
};

}  // namespace rpc::server
