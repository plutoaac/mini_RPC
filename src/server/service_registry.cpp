#include "server/service_registry.h"

#include <utility>

namespace rpc::server {

std::string ServiceRegistry::BuildKey(std::string_view service_name,
                                      std::string_view method_name) {
  std::string key;
  key.reserve(service_name.size() + method_name.size() + 1);
  key.append(service_name);
  key.push_back('.');
  key.append(method_name);
  return key;
}

bool ServiceRegistry::Register(std::string_view service_name,
                               std::string_view method_name, Handler handler) {
  if (service_name.empty() || method_name.empty() || !handler) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  if (handlers_.contains(key)) {
    return false;
  }
  return handlers_.emplace(key, std::move(handler)).second;
}

std::optional<Handler> ServiceRegistry::Find(
    std::string_view service_name, std::string_view method_name) const {
  std::scoped_lock lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  const auto it = handlers_.find(key);
  if (it == handlers_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace rpc::server
