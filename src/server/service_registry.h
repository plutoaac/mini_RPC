#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rpc::server {

enum class RpcStatusCode {
  kOk = 0,
  kMethodNotFound = 1,
  kParseError = 2,
  kInternalError = 3,
};

// Framework exception type used by handlers to return business-level errors.
class RpcError : public std::runtime_error {
 public:
  RpcError(RpcStatusCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  [[nodiscard]] RpcStatusCode code() const noexcept { return code_; }

 private:
  RpcStatusCode code_;
};

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
