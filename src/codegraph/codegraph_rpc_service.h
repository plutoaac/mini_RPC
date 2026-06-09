#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/service_registry.h"

namespace rpc::codegraph {

struct ToolSpec {
  std::string mcp_name;
  std::string rpc_method;
  std::string description;
  nlohmann::json input_schema;
};

[[nodiscard]] const std::vector<ToolSpec>& CodeGraphToolSpecs();
[[nodiscard]] const ToolSpec* FindToolByMcpName(std::string_view name);
[[nodiscard]] const ToolSpec* FindToolByRpcMethod(std::string_view method);

class CodeGraphBackend {
 public:
  virtual ~CodeGraphBackend() = default;

  // Implementations must be safe to call concurrently from mini_rpc business
  // worker threads.
  [[nodiscard]] virtual nlohmann::json Invoke(std::string_view method,
                                              const nlohmann::json& args) = 0;
};

class EmptyCodeGraphBackend final : public CodeGraphBackend {
 public:
  [[nodiscard]] nlohmann::json Invoke(std::string_view method,
                                      const nlohmann::json& args) override;
};

class SQLiteCodeGraphBackend final : public CodeGraphBackend {
 public:
  explicit SQLiteCodeGraphBackend(std::string db_path);

  [[nodiscard]] nlohmann::json Invoke(std::string_view method,
                                      const nlohmann::json& args) override;

 private:
  std::string db_path_;
};

// Registers CodeGraph RPC handlers in `registry`.
// IMPORTANT: `backend` is captured by reference. The caller must ensure that
// `backend` outlives `registry` and any RPC server that dispatches through it.
[[nodiscard]] bool RegisterCodeGraphService(rpc::server::ServiceRegistry& registry,
                                            CodeGraphBackend& backend);

}  // namespace rpc::codegraph
