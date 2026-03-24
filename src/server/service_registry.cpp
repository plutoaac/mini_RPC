#include "server/service_registry.h"

#include <utility>

namespace rpc::server {

std::string ServiceRegistry::BuildKey(std::string_view service_name,
                                      std::string_view method_name) {
  // 预分配容量，减少字符串扩容。
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

  // 注册表可能在并发场景下访问，统一加锁保护。
  std::scoped_lock lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  if (handlers_.contains(key)) {
    return false;
  }
  return handlers_.emplace(key, std::move(handler)).second;
}

std::optional<std::reference_wrapper<const Handler>> ServiceRegistry::Find(
    std::string_view service_name, std::string_view method_name) const {
  // 查找与注册共享同一把锁，保证线程安全。
  std::scoped_lock lock(mutex_);
  const std::string key = BuildKey(service_name, method_name);
  const auto it = handlers_.find(key);
  if (it == handlers_.end()) {
    return std::nullopt;
  }
  return std::cref(it->second);
}

}  // namespace rpc::server
