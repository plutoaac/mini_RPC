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

// 复用 common 错误体系，避免框架内部出现多套错误定义。
using RpcStatusCode = rpc::common::ErrorCode;
using RpcError = rpc::common::RpcException;

// 通用 handler：输入请求 payload（二进制），输出响应 payload（二进制）。
using Handler = std::function<std::string(std::string_view request_payload)>;

class ServiceRegistry {
 public:
  // 注册 service + method 到 handler。
  // 返回 false 代表参数非法或重复注册。
  [[nodiscard]] bool Register(std::string_view service_name,
                              std::string_view method_name, Handler handler);

  // 查询 handler；找不到返回 nullopt。
  [[nodiscard]] std::optional<Handler> Find(std::string_view service_name,
                                            std::string_view method_name) const;

 private:
  // 统一 key 格式：service.method
  [[nodiscard]] static std::string BuildKey(std::string_view service_name,
                                            std::string_view method_name);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace rpc::server
