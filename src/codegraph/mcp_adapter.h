#pragma once

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <string>

#include <nlohmann/json.hpp>

#include "client/rpc_client.h"

namespace rpc::codegraph {

struct McpAdapterOptions {
  std::string host{"127.0.0.1"};
  std::uint16_t port{50061};
  std::chrono::milliseconds rpc_timeout{3000};
};

class McpAdapter {
 public:
  explicit McpAdapter(McpAdapterOptions options);

  int Run(std::istream& input, std::ostream& output);
  [[nodiscard]] nlohmann::json HandleRequest(const nlohmann::json& request);

 private:
  [[nodiscard]] nlohmann::json HandleInitialize(const nlohmann::json& request);
  [[nodiscard]] nlohmann::json HandleToolsList(const nlohmann::json& request);
  [[nodiscard]] nlohmann::json HandleToolsCall(const nlohmann::json& request);

  [[nodiscard]] nlohmann::json MakeResponse(const nlohmann::json& id,
                                            nlohmann::json result) const;
  [[nodiscard]] nlohmann::json MakeError(const nlohmann::json& id, int code,
                                         std::string message) const;
  [[nodiscard]] nlohmann::json MakeToolResult(std::string text,
                                              bool is_error) const;

  McpAdapterOptions options_;
  rpc::client::RpcClient client_;
};

}  // namespace rpc::codegraph

