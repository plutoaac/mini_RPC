#include "server/service_registry.h"

#include <utility>

namespace rpc::server {

std::string ServiceRegistry::BuildKey(const std::string& service_name,
                                      const std::string& method_name) {
  return service_name + "." + method_name;
}

bool ServiceRegistry::Register(std::string service_name,
                               std::string method_name, Handler handler) {
  if (service_name.empty() || method_name.empty() || !handler) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  return handlers_.emplace(key, std::move(handler)).second;
}

bool ServiceRegistry::Find(const std::string& service_name,
                           const std::string& method_name,
                           Handler* out_handler) const {
  if (out_handler == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  const auto it = handlers_.find(key);
  if (it == handlers_.end()) {
    return false;
  }

  *out_handler = it->second;
  return true;
}

}  // namespace rpc::server
