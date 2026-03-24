#pragma once

#include <cstdint>

#include "common/unique_fd.h"
#include "server/service_registry.h"

namespace rpc::server {

// 阻塞式 RPC 服务端：负责连接接入、请求分发与响应返回。
class RpcServer {
 public:
  RpcServer(std::uint16_t port, const ServiceRegistry& registry);

  // 启动阻塞 accept 循环。
  bool Start();

 private:
  // 处理单个连接上的请求循环。
  bool HandleClient(const rpc::common::UniqueFd& client_fd) const;

  std::uint16_t port_;
  const ServiceRegistry& registry_;
};

}  // namespace rpc::server
