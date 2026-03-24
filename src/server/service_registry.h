#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/rpc_error.h"

namespace rpc::server {

using RpcStatusCode = rpc::common::ErrorCode;
using RpcError = rpc::common::RpcException;

// Generic handler: payload bytes in, payload bytes out.
using Handler = std::function<std::string(std::string_view request_payload)>;

class ServiceRegistry {
 public:
  [[nodiscard]] bool Register(std::string_view service_name,
                              std::string_view method_name, Handler handler);

  [[nodiscard]] std::optional<Handler> Find(std::string_view service_name,
                                            std::string_view method_name) const;

 private:
  [[nodiscard]] static std::string BuildKey(std::string_view service_name,
                                            std::string_view method_name);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace rpc::server
