#include "codegraph/codegraph_rpc_service.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/rpc_error.h"

namespace rpc::codegraph {
namespace {

using Json = nlohmann::json;

Json ObjectSchema(Json properties, Json required = Json::array()) {
  Json schema;
  schema["type"] = "object";
  schema["properties"] = std::move(properties);
  if (!required.empty()) {
    schema["required"] = std::move(required);
  }
  return schema;
}

Json StringProperty(std::string description) {
  return Json{{"type", "string"}, {"description", std::move(description)}};
}

Json IntegerProperty(std::string description, int default_value) {
  return Json{{"type", "integer"},
              {"description", std::move(description)},
              {"default", default_value}};
}

rpc::server::RpcError ParseError(std::string message) {
  return rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                               std::move(message));
}

std::string RequiredString(const Json& args, std::string_view method,
                           std::string_view key) {
  const auto it = args.find(std::string(key));
  if (it == args.end() || !it->is_string() || it->get<std::string>().empty()) {
    throw ParseError("CodeGraph." + std::string(method) +
                     " requires non-empty string field '" + std::string(key) +
                     "'");
  }
  return it->get<std::string>();
}

int OptionalInt(const Json& args, std::string_view method, std::string_view key,
                int default_value, int min_value, int max_value) {
  const auto it = args.find(std::string(key));
  if (it == args.end() || it->is_null()) {
    return default_value;
  }
  if (!it->is_number_integer()) {
    throw ParseError("CodeGraph." + std::string(method) +
                     " field '" + std::string(key) + "' must be an integer");
  }

  const int value = it->get<int>();
  if (value < min_value || value > max_value) {
    throw ParseError("CodeGraph." + std::string(method) +
                     " field '" + std::string(key) + "' is out of range");
  }
  return value;
}

Json EmptyBackendEnvelope(std::string_view method, const Json& args) {
  return Json{{"backend", "empty"},
              {"method", std::string(method)},
              {"args", args},
              {"warning",
               "CodeGraph backend is not configured yet; transport and MCP "
               "adapter are ready"}};
}

Json EmptyNodeResult(std::string_view method, const Json& args) {
  Json result = EmptyBackendEnvelope(method, args);
  result["nodes"] = Json::array();
  result["edges"] = Json::array();
  return result;
}

std::string JsonPayloadToString(std::string_view payload) {
  return std::string(payload.data(), payload.size());
}

Json ParsePayload(std::string_view method, std::string_view payload) {
  if (payload.empty()) {
    return Json::object();
  }

  Json parsed;
  try {
    const std::string raw = JsonPayloadToString(payload);
    parsed = Json::parse(raw);
  } catch (const std::exception& ex) {
    throw ParseError("CodeGraph." + std::string(method) +
                     " received invalid JSON payload: " + ex.what());
  }

  if (!parsed.is_object()) {
    throw ParseError("CodeGraph." + std::string(method) +
                     " payload must be a JSON object");
  }
  return parsed;
}

std::string HandleRpcCall(CodeGraphBackend& backend, std::string method,
                          std::string_view payload) {
  const Json args = ParsePayload(method, payload);
  return backend.Invoke(method, args).dump(2);
}

}  // namespace

const std::vector<ToolSpec>& CodeGraphToolSpecs() {
  static const std::vector<ToolSpec> specs = {
      ToolSpec{
          .mcp_name = "codegraph_search",
          .rpc_method = "Search",
          .description =
              "Search for code symbols by name and return matching symbols.",
          .input_schema =
              ObjectSchema(
                  Json{{"query", StringProperty("Symbol name or search query")},
                       {"limit", IntegerProperty("Max results", 20)}},
                  Json::array({"query"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_context",
          .rpc_method = "Context",
          .description =
              "Get rich context for a symbol: definition, callers and callees.",
          .input_schema =
              ObjectSchema(Json{{"symbol", StringProperty("Symbol name")},
                                {"limit", IntegerProperty("Max results", 10)}},
                           Json::array({"symbol"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_callers",
          .rpc_method = "Callers",
          .description = "Find callers of a function or method.",
          .input_schema = ObjectSchema(
              Json{{"symbol", StringProperty("Symbol name")},
                   {"max_depth", IntegerProperty("Max traversal depth", 3)}},
              Json::array({"symbol"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_callees",
          .rpc_method = "Callees",
          .description = "Find callees of a function or method.",
          .input_schema = ObjectSchema(
              Json{{"symbol", StringProperty("Symbol name")},
                   {"max_depth", IntegerProperty("Max traversal depth", 3)}},
              Json::array({"symbol"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_impact",
          .rpc_method = "Impact",
          .description = "Analyze the impact radius of changing a symbol.",
          .input_schema = ObjectSchema(
              Json{{"symbol", StringProperty("Symbol name")},
                   {"max_depth", IntegerProperty("Max traversal depth", 5)}},
              Json::array({"symbol"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_node",
          .rpc_method = "Node",
          .description = "Get details for a single symbol.",
          .input_schema =
              ObjectSchema(Json{{"symbol", StringProperty("Symbol name")}},
                           Json::array({"symbol"})),
      },
      ToolSpec{
          .mcp_name = "codegraph_status",
          .rpc_method = "Status",
          .description = "Get CodeGraph service and index statistics.",
          .input_schema = ObjectSchema(Json::object()),
      },
      ToolSpec{
          .mcp_name = "codegraph_files",
          .rpc_method = "Files",
          .description = "List indexed files.",
          .input_schema = ObjectSchema(Json::object()),
      },
  };
  return specs;
}

const ToolSpec* FindToolByMcpName(std::string_view name) {
  const auto& specs = CodeGraphToolSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(), [&](const auto& spec) {
    return spec.mcp_name == name;
  });
  return it == specs.end() ? nullptr : &*it;
}

const ToolSpec* FindToolByRpcMethod(std::string_view method) {
  const auto& specs = CodeGraphToolSpecs();
  const auto it = std::find_if(specs.begin(), specs.end(), [&](const auto& spec) {
    return spec.rpc_method == method;
  });
  return it == specs.end() ? nullptr : &*it;
}

nlohmann::json EmptyCodeGraphBackend::Invoke(std::string_view method,
                                             const nlohmann::json& args) {
  if (method == "Status") {
    Json tools = Json::array();
    for (const auto& spec : CodeGraphToolSpecs()) {
      tools.push_back({{"mcp_name", spec.mcp_name},
                       {"rpc_method", "CodeGraph." + spec.rpc_method}});
    }
    return Json{{"service", "CodeGraph"},
                {"backend", "empty"},
                {"ready", true},
                {"transport", "mini_rpc"},
                {"tools", std::move(tools)},
                {"note",
                 "MVP transport is live; attach a real CodeGraph backend next"}};
  }

  if (method == "Search") {
    const std::string query = RequiredString(args, method, "query");
    const int limit = OptionalInt(args, method, "limit", 20, 1, 200);
    Json result = EmptyBackendEnvelope(method, args);
    result["query"] = query;
    result["limit"] = limit;
    result["nodes"] = Json::array();
    return result;
  }

  if (method == "Context") {
    const std::string symbol = RequiredString(args, method, "symbol");
    (void)OptionalInt(args, method, "limit", 10, 1, 200);
    Json result = EmptyBackendEnvelope(method, args);
    result["symbol"] = symbol;
    result["callers"] = Json::array();
    result["callees"] = Json::array();
    result["edges"] = Json::array();
    return result;
  }

  if (method == "Callers" || method == "Callees" || method == "Impact") {
    const std::string symbol = RequiredString(args, method, "symbol");
    (void)OptionalInt(args, method, "max_depth", method == "Impact" ? 5 : 3, 1,
                      20);
    Json result = EmptyNodeResult(method, args);
    result["symbol"] = symbol;
    return result;
  }

  if (method == "Node") {
    const std::string symbol = RequiredString(args, method, "symbol");
    Json result = EmptyBackendEnvelope(method, args);
    result["symbol"] = symbol;
    result["node"] = nullptr;
    return result;
  }

  if (method == "Files") {
    Json result = EmptyBackendEnvelope(method, args);
    result["files"] = Json::array();
    return result;
  }

  throw rpc::server::RpcError(rpc::server::RpcStatusCode::kMethodNotFound,
                              "unknown CodeGraph method: " +
                                  std::string(method));
}

bool RegisterCodeGraphService(rpc::server::ServiceRegistry& registry,
                              CodeGraphBackend& backend) {
  bool ok = true;
  for (const auto& spec : CodeGraphToolSpecs()) {
    const std::string method = spec.rpc_method;
    ok = registry.Register("CodeGraph", method,
                           [&backend, method](std::string_view payload) {
                             return HandleRpcCall(backend, method, payload);
                           }) &&
         ok;
  }
  return ok;
}

}  // namespace rpc::codegraph
