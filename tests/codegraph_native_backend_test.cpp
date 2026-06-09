// End-to-end integration test for CodeGraphNativeBackend.
// Tests the full chain: codegraph-cpp Database → NativeBackend → RPC server → MCP adapter

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "client/rpc_client.h"
#include "codegraph/codegraph_native_backend.h"
#include "codegraph/codegraph_rpc_service.h"
#include "codegraph/mcp_adapter.h"
#include "codegraph/db/database.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

using Json = nlohmann::json;

bool CanConnect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool WaitServerReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

Json MakeJsonRpc(int id, std::string method, Json params = Json::object()) {
  return Json{{"jsonrpc", "2.0"}, {"id", id},
              {"method", std::move(method)}, {"params", std::move(params)}};
}

Json ToolCall(int id, std::string name, Json arguments = Json::object()) {
  return MakeJsonRpc(id, "tools/call",
                     Json{{"name", std::move(name)}, {"arguments", std::move(arguments)}});
}

std::string ToolText(const Json& response) {
  return response.at("result").at("content").at(0).at("text").get<std::string>();
}

Json ParseJsonOrDie(const std::string& payload, const char* label) {
  if (payload.empty()) {
    std::cerr << label << " returned empty JSON\n";
    assert(false);
  }
  try {
    return Json::parse(payload);
  } catch (const std::exception& ex) {
    std::cerr << label << " invalid JSON: " << ex.what() << "\n" << payload << "\n";
    assert(false);
  }
  return Json::object();
}

// Create a fixture database using codegraph-cpp's Database API
void CreateFixtureDb(const char* path) {
  std::remove(path);
  codegraph::Database db(path);
  db.init_schema();

  db.begin_transaction();

  // Insert nodes
  codegraph::Node n1;
  n1.kind = codegraph::NodeKind::Function;
  n1.name = "caller";
  n1.qualified_name = "fixture::caller";
  n1.file_path = "/tmp/fixture.cpp";
  n1.language = "cpp";
  n1.line = 1; n1.col = 1; n1.end_line = 3; n1.end_col = 1;
  n1.signature = "void caller()";
  int64_t id1 = db.insert_node(n1);

  codegraph::Node n2;
  n2.kind = codegraph::NodeKind::Function;
  n2.name = "target";
  n2.qualified_name = "fixture::target";
  n2.file_path = "/tmp/fixture.cpp";
  n2.language = "cpp";
  n2.line = 5; n2.col = 1; n2.end_line = 7; n2.end_col = 1;
  n2.signature = "void target()";
  int64_t id2 = db.insert_node(n2);

  codegraph::Node n3;
  n3.kind = codegraph::NodeKind::Function;
  n3.name = "callee";
  n3.qualified_name = "fixture::callee";
  n3.file_path = "/tmp/fixture.cpp";
  n3.language = "cpp";
  n3.line = 9; n3.col = 1; n3.end_line = 11; n3.end_col = 1;
  n3.signature = "void callee()";
  int64_t id3 = db.insert_node(n3);

  // Insert call edges: caller -> target -> callee
  codegraph::Edge e1;
  e1.source_id = id1; e1.target_id = id2;
  e1.kind = codegraph::EdgeKind::Calls; e1.line = 2;
  db.insert_edge(e1);

  codegraph::Edge e2;
  e2.source_id = id2; e2.target_id = id3;
  e2.kind = codegraph::EdgeKind::Calls; e2.line = 6;
  db.insert_edge(e2);

  // Insert file record
  codegraph::FileRecord fr;
  fr.path = "/tmp/fixture.cpp";
  fr.language = "cpp";
  fr.mtime = 0;
  fr.size = 100;
  db.insert_file(fr);

  db.commit();
}

}  // namespace

int main() {
  constexpr std::uint16_t kPort = 50171;
  const char* fixture_path = "/tmp/codegraph_native_test.db";

  // 1. Create fixture using codegraph-cpp
  CreateFixtureDb(fixture_path);

  // 2. Create native backend
  rpc::server::ServiceRegistry registry;
  rpc::codegraph::CodeGraphNativeBackend native_backend(fixture_path);
  assert(rpc::codegraph::RegisterCodeGraphService(registry, native_backend));

  // 3. Start RPC server
  rpc::server::RpcServer server(kPort, registry, 2U, 2U);
  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });
  assert(WaitServerReady(kPort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 4. Test via MCP adapter (full chain: MCP → RPC → NativeBackend → codegraph-cpp)
  rpc::codegraph::McpAdapter adapter({
    .host = "127.0.0.1",
    .port = kPort,
    .rpc_timeout = std::chrono::milliseconds(2000)
  });

  // Test Status
  const Json status_response = adapter.HandleRequest(ToolCall(1, "codegraph_status"));
  const Json status_json = ParseJsonOrDie(ToolText(status_response), "Status");
  assert(status_json.at("backend") == "native");
  assert(status_json.at("ready") == true);
  std::cout << "  [PASS] Status: backend=native, ready=true\n";

  // Test Search
  const Json search_response = adapter.HandleRequest(
      ToolCall(2, "codegraph_search", Json{{"query", "target"}, {"limit", 5}}));
  const Json search_json = ParseJsonOrDie(ToolText(search_response), "Search");
  assert(search_json.at("backend") == "native");
  assert(search_json.at("nodes").is_array());
  assert(!search_json.at("nodes").empty());
  assert(search_json.at("nodes").at(0).at("name") == "target");
  std::cout << "  [PASS] Search: found 'target' node\n";

  // Test Context (the key integration test)
  const Json context_response = adapter.HandleRequest(
      ToolCall(3, "codegraph_context", Json{{"symbol", "target"}}));
  const Json context_json = ParseJsonOrDie(ToolText(context_response), "Context");
  assert(context_json.at("backend") == "native");
  assert(context_json.at("symbol").at("name") == "target");

  // Verify callers: "caller" calls "target"
  assert(context_json.at("callers").is_array());
  bool found_caller = false;
  for (const auto& c : context_json.at("callers")) {
    if (c.at("name") == "caller") { found_caller = true; break; }
  }
  assert(found_caller);
  std::cout << "  [PASS] Context: 'target' has caller 'caller'\n";

  // Verify callees: "target" calls "callee"
  assert(context_json.at("callees").is_array());
  bool found_callee = false;
  for (const auto& c : context_json.at("callees")) {
    if (c.at("name") == "callee") { found_callee = true; break; }
  }
  assert(found_callee);
  std::cout << "  [PASS] Context: 'target' has callee 'callee'\n";

  // Test Callers
  const Json callers_response = adapter.HandleRequest(
      ToolCall(4, "codegraph_callers", Json{{"symbol", "callee"}}));
  const Json callers_json = ParseJsonOrDie(ToolText(callers_response), "Callers");
  assert(callers_json.at("backend") == "native");
  bool found_target_as_caller = false;
  for (const auto& n : callers_json.at("nodes")) {
    if (n.at("name") == "target") { found_target_as_caller = true; break; }
  }
  assert(found_target_as_caller);
  std::cout << "  [PASS] Callers: 'callee' is called by 'target'\n";

  // Test Callees
  const Json callees_response = adapter.HandleRequest(
      ToolCall(5, "codegraph_callees", Json{{"symbol", "caller"}}));
  const Json callees_json = ParseJsonOrDie(ToolText(callees_response), "Callees");
  assert(callees_json.at("backend") == "native");
  bool found_target_as_callee = false;
  for (const auto& n : callees_json.at("nodes")) {
    if (n.at("name") == "target") { found_target_as_callee = true; break; }
  }
  assert(found_target_as_callee);
  std::cout << "  [PASS] Callees: 'caller' calls 'target'\n";

  // Test direct RPC client (bypass MCP adapter)
  rpc::client::RpcClient direct_client(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(1000)});
  const auto direct_status = direct_client.Call("CodeGraph", "Status", "{}");
  assert(direct_status.ok());
  const Json direct_json = ParseJsonOrDie(direct_status.response_payload, "Direct RPC");
  assert(direct_json.at("backend") == "native");
  assert(direct_json.at("node_count") == 3);
  assert(direct_json.at("edge_count") == 2);
  std::cout << "  [PASS] Direct RPC: node_count=3, edge_count=2\n";

  // Cleanup
  server.Stop();
  if (server_thread.joinable()) server_thread.join();
  assert(start_result);
  std::remove(fixture_path);

  std::cout << "codegraph_native_backend_test passed\n";
  return 0;
}
