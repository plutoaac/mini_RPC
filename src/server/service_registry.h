#pragma once

#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
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
using Handler = std::function<std::string(const std::string& request_payload)>;

class ServiceRegistry {
 public:
  bool Register(std::string service_name, std::string method_name,
                Handler handler);

  bool Find(const std::string& service_name, const std::string& method_name,
            Handler* out_handler) const;

 private:
  static std::string BuildKey(const std::string& service_name,
                              const std::string& method_name);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace rpc::server
