#include "codegraph/mcp_adapter.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "codegraph/codegraph_rpc_service.h"

namespace rpc::codegraph {
namespace {

using Json = nlohmann::json;

Json JsonRpcId(const Json& request) {
  return request.contains("id") ? request.at("id") : Json(nullptr);
}

bool IsNotification(const Json& request) {
  return !request.contains("id") || request.at("id").is_null();
}

std::string RequiredStringParam(const Json& params, std::string_view key) {
  const auto it = params.find(std::string(key));
  if (it == params.end() || !it->is_string() || it->get<std::string>().empty()) {
    throw std::invalid_argument("missing non-empty string param '" +
                                std::string(key) + "'");
  }
  return it->get<std::string>();
}

Json RequestParamsObject(const Json& request) {
  if (!request.contains("params") || request.at("params").is_null()) {
    return Json::object();
  }
  if (!request.at("params").is_object()) {
    throw std::invalid_argument("params must be an object");
  }
  return request.at("params");
}

std::string RpcFailureText(const rpc::client::RpcCallResult& result) {
  return "RPC error: code=" + std::to_string(result.status.code.value()) +
         " category=" + result.status.code.category().name() +
         " message=" + result.status.message;
}

}  // namespace

McpAdapter::McpAdapter(McpAdapterOptions options)
    : options_(std::move(options)),
      client_(options_.host, options_.port,
              {.send_timeout = options_.rpc_timeout,
               .recv_timeout = options_.rpc_timeout,
               .heartbeat_interval = std::chrono::seconds(30),
               .heartbeat_timeout = std::chrono::seconds(45)}) {}

int McpAdapter::Run(std::istream& input, std::ostream& output) {
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }

    Json response;
    try {
      const Json request = Json::parse(line);
      if (IsNotification(request)) {
        continue;
      }
      response = HandleRequest(request);
    } catch (const Json::parse_error& ex) {
      response = MakeError(nullptr, -32700,
                           std::string("Parse error: ") + ex.what());
    } catch (const std::invalid_argument& ex) {
      response = MakeError(nullptr, -32600,
                           std::string("Invalid request: ") + ex.what());
    } catch (const std::exception& ex) {
      response =
          MakeError(nullptr, -32603, std::string("Internal error: ") + ex.what());
    }

    output << response.dump() << '\n';
    output.flush();
  }
  return 0;
}

nlohmann::json McpAdapter::HandleRequest(const nlohmann::json& request) {
  if (!request.is_object()) {
    throw std::invalid_argument("request must be an object");
  }

  const Json id = JsonRpcId(request);
  const std::string method = request.value("method", "");
  if (method.empty()) {
    return MakeError(id, -32600, "missing method");
  }

  if (method == "initialize") {
    return HandleInitialize(request);
  }
  if (method == "tools/list") {
    return HandleToolsList(request);
  }
  if (method == "tools/call") {
    return HandleToolsCall(request);
  }
  if (method == "notifications/initialized") {
    return MakeResponse(id, Json::object());
  }

  return MakeError(id, -32601, "Method not found: " + method);
}

nlohmann::json McpAdapter::HandleInitialize(const nlohmann::json& request) {
  return MakeResponse(
      JsonRpcId(request),
      Json{{"protocolVersion", "2024-11-05"},
           {"capabilities", Json{{"tools", Json::object()}}},
           {"serverInfo",
            Json{{"name", "codegraph-mini-rpc-adapter"},
                 {"version", "0.1.0"}}}});
}

nlohmann::json McpAdapter::HandleToolsList(const nlohmann::json& request) {
  Json tools = Json::array();
  for (const auto& spec : CodeGraphToolSpecs()) {
    tools.push_back(Json{{"name", spec.mcp_name},
                         {"description", spec.description},
                         {"inputSchema", spec.input_schema}});
  }
  return MakeResponse(JsonRpcId(request), Json{{"tools", std::move(tools)}});
}

nlohmann::json McpAdapter::HandleToolsCall(const nlohmann::json& request) {
  const Json id = JsonRpcId(request);
  Json params;
  try {
    params = RequestParamsObject(request);
  } catch (const std::invalid_argument& ex) {
    return MakeError(id, -32602, ex.what());
  }

  std::string tool_name;
  try {
    tool_name = RequiredStringParam(params, "name");
  } catch (const std::invalid_argument& ex) {
    return MakeError(id, -32602, ex.what());
  }

  const ToolSpec* spec = FindToolByMcpName(tool_name);
  if (spec == nullptr) {
    return MakeResponse(id, MakeToolResult("Unknown tool: " + tool_name, true));
  }

  Json arguments = Json::object();
  if (params.contains("arguments") && !params.at("arguments").is_null()) {
    if (!params.at("arguments").is_object()) {
      return MakeError(id, -32602, "arguments must be an object");
    }
    arguments = params.at("arguments");
  }

  const std::string payload = arguments.dump();
  const auto result = client_.Call("CodeGraph", spec->rpc_method, payload);
  if (!result.ok()) {
    return MakeResponse(id, MakeToolResult(RpcFailureText(result), true));
  }

  return MakeResponse(id, MakeToolResult(result.response_payload, false));
}

nlohmann::json McpAdapter::MakeResponse(const nlohmann::json& id,
                                        nlohmann::json result) const {
  return Json{{"jsonrpc", "2.0"}, {"result", std::move(result)}, {"id", id}};
}

nlohmann::json McpAdapter::MakeError(const nlohmann::json& id, int code,
                                     std::string message) const {
  return Json{{"jsonrpc", "2.0"},
              {"error", Json{{"code", code}, {"message", std::move(message)}}},
              {"id", id}};
}

nlohmann::json McpAdapter::MakeToolResult(std::string text,
                                          bool is_error) const {
  Json result{{"content", Json::array({Json{{"type", "text"},
                                             {"text", std::move(text)}}})}};
  if (is_error) {
    result["isError"] = true;
  }
  return result;
}

}  // namespace rpc::codegraph
