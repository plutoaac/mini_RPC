#include <arpa/inet.h>
#include <netinet/in.h>
#include <sqlite3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "client/rpc_client.h"
#include "codegraph/codegraph_rpc_service.h"
#include "codegraph/mcp_adapter.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

using Json = nlohmann::json;

std::uint16_t PickUnusedPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(0);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  socklen_t len = sizeof(addr);
  assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  const auto port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
  ::close(fd);
  return port;
}

std::string TempDbPath(const char* name) {
  return std::string("/tmp/codegraph_mcp_rpc_") + std::to_string(::getpid()) +
         "_" + name + ".db";
}

bool CanConnect(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  (void)::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  const bool ok =
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

bool WaitServerReady(std::uint16_t port, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < timeout) {
    if (CanConnect(port)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

Json MakeJsonRpc(int id, std::string method, Json params = Json::object()) {
  return Json{{"jsonrpc", "2.0"},
              {"id", id},
              {"method", std::move(method)},
              {"params", std::move(params)}};
}

Json ToolCall(int id, std::string name, Json arguments = Json::object()) {
  return MakeJsonRpc(id, "tools/call",
                     Json{{"name", std::move(name)},
                          {"arguments", std::move(arguments)}});
}

std::string ToolText(const Json& response) {
  return response.at("result")
      .at("content")
      .at(0)
      .at("text")
      .get<std::string>();
}

bool HasTool(const Json& tools_response, const std::string& tool_name) {
  for (const auto& tool : tools_response.at("result").at("tools")) {
    if (tool.at("name").get<std::string>() == tool_name) {
      return true;
    }
  }
  return false;
}

Json ParseJsonOrDie(const std::string& payload, const char* label) {
  if (payload.empty()) {
    std::cerr << label << " returned an empty JSON payload\n";
    assert(false);
  }
  try {
    return Json::parse(payload);
  } catch (const std::exception& ex) {
    std::cerr << label << " returned invalid JSON: " << ex.what() << '\n'
              << payload << '\n';
    assert(false);
  }
  return Json::object();
}

void Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::cerr << (err == nullptr ? "sqlite exec failed" : err) << '\n';
    sqlite3_free(err);
    assert(false);
  }
}

void CreateFixtureDb(const char* path) {
  std::remove(path);
  sqlite3* raw = nullptr;
  assert(sqlite3_open(path, &raw) == SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);

  Exec(db.get(), R"SQL(
CREATE TABLE files (
  path TEXT PRIMARY KEY,
  language TEXT NOT NULL,
  size INTEGER DEFAULT 0,
  mtime INTEGER DEFAULT 0,
  indexed_at INTEGER DEFAULT 0,
  content_hash TEXT
);
CREATE TABLE nodes (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  qualified_name TEXT NOT NULL,
  file_path TEXT NOT NULL,
  language TEXT NOT NULL,
  start_line INTEGER NOT NULL,
  end_line INTEGER NOT NULL,
  start_column INTEGER NOT NULL,
  end_column INTEGER NOT NULL,
  docstring TEXT,
  signature TEXT,
  visibility TEXT,
  is_exported INTEGER DEFAULT 0,
  is_async INTEGER DEFAULT 0,
  is_static INTEGER DEFAULT 0,
  is_abstract INTEGER DEFAULT 0,
  decorators TEXT,
  type_parameters TEXT,
  updated_at INTEGER NOT NULL
);
CREATE TABLE edges (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  target TEXT NOT NULL,
  kind TEXT NOT NULL,
  metadata TEXT,
  line INTEGER,
  col INTEGER,
  provenance TEXT DEFAULT NULL
);
)SQL");

  Exec(db.get(), R"SQL(
INSERT INTO files(path, language) VALUES('/tmp/fixture.cpp', 'cpp');
INSERT INTO nodes(id, kind, name, qualified_name, file_path, language, start_line, end_line, start_column, end_column, signature, docstring, is_static, is_exported, updated_at)
VALUES
  ('caller-id', 'function', 'caller', 'fixture::caller', '/tmp/fixture.cpp', 'cpp', 1, 3, 1, 1, 'void caller()', '', 0, 0, 1),
  ('target-id', 'function', 'target', 'fixture::target', '/tmp/fixture.cpp', 'cpp', 5, 7, 1, 1, 'void target()', '', 0, 0, 1),
  ('callee-id', 'function', 'callee', 'fixture::callee', '/tmp/fixture.cpp', 'cpp', 9, 11, 1, 1, 'void callee()', '', 0, 0, 1);
INSERT INTO edges(source, target, kind, line, col, metadata) VALUES
  ('caller-id', 'target-id', 'calls', 2, 3, '{}'),
  ('target-id', 'callee-id', 'calls', 6, 3, '{}');
)SQL");
}

void CreateCodeGraphCppFixtureDb(const char* path) {
  std::remove(path);
  sqlite3* raw = nullptr;
  assert(sqlite3_open(path, &raw) == SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);

  Exec(db.get(), R"SQL(
CREATE TABLE nodes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  kind INTEGER NOT NULL,
  name TEXT NOT NULL,
  qualified_name TEXT,
  file_path TEXT NOT NULL,
  language TEXT,
  line INTEGER,
  col INTEGER,
  end_line INTEGER,
  end_col INTEGER,
  signature TEXT,
  docstring TEXT,
  visibility TEXT,
  is_static INTEGER DEFAULT 0,
  is_const INTEGER DEFAULT 0,
  is_exported INTEGER DEFAULT 0
);
CREATE TABLE edges (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source_id INTEGER NOT NULL,
  target_id INTEGER NOT NULL,
  kind INTEGER NOT NULL,
  line INTEGER,
  col INTEGER,
  metadata TEXT
);
CREATE TABLE files (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  path TEXT UNIQUE NOT NULL,
  language TEXT,
  mtime INTEGER,
  size INTEGER
);
)SQL");

  Exec(db.get(), R"SQL(
INSERT INTO files(path, language, mtime, size)
VALUES('/tmp/fixture.cpp', 'cpp', 1, 100);
INSERT INTO nodes(kind, name, qualified_name, file_path, language, line, col, end_line, end_col, signature, docstring, is_static, is_exported)
VALUES
  (1, 'caller', 'fixture::caller', '/tmp/fixture.cpp', 'cpp', 1, 1, 3, 1, 'void caller()', '', 0, 0),
  (1, 'target', 'fixture::target', '/tmp/fixture.cpp', 'cpp', 5, 1, 7, 1, 'void target()', '', 0, 0),
  (1, 'callee', 'fixture::callee', '/tmp/fixture.cpp', 'cpp', 9, 1, 11, 1, 'void callee()', '', 0, 0);
INSERT INTO edges(source_id, target_id, kind, line, col, metadata) VALUES
  (1, 2, 1, 2, 3, '{}'),
  (2, 3, 1, 6, 3, '{}');
)SQL");
}

}  // namespace

int main() {
  const std::uint16_t kPort = PickUnusedPort();

  rpc::server::ServiceRegistry registry;
  rpc::codegraph::EmptyCodeGraphBackend backend;
  assert(rpc::codegraph::RegisterCodeGraphService(registry, backend));

  rpc::server::RpcServer server(kPort, registry, 2U, 2U);
  bool start_result = false;
  std::thread server_thread([&]() { start_result = server.Start(); });

  assert(WaitServerReady(kPort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rpc::client::RpcClient direct_client(
      "127.0.0.1", kPort,
      {.send_timeout = std::chrono::milliseconds(1000),
       .recv_timeout = std::chrono::milliseconds(1000)});
  const auto direct_status = direct_client.Call("CodeGraph", "Status", "{}");
  assert(direct_status.ok());
  const Json direct_status_json =
      ParseJsonOrDie(direct_status.response_payload, "direct Status");
  assert(direct_status_json.at("backend") == "empty");
  assert(direct_status_json.at("transport") == "mini_rpc");

  rpc::codegraph::McpAdapter adapter(
      {.host = "127.0.0.1",
       .port = kPort,
       .rpc_timeout = std::chrono::milliseconds(1000)});

  const Json init_response = adapter.HandleRequest(
      MakeJsonRpc(1, "initialize", Json{{"protocolVersion", "2024-11-05"}}));
  assert(init_response.at("result").at("serverInfo").at("name") ==
         "codegraph-mini-rpc-adapter");

  const Json tools_response = adapter.HandleRequest(MakeJsonRpc(2, "tools/list"));
  assert(HasTool(tools_response, "codegraph_status"));
  assert(HasTool(tools_response, "codegraph_search"));

  const Json status_response =
      adapter.HandleRequest(ToolCall(3, "codegraph_status"));
  const Json status_json =
      ParseJsonOrDie(ToolText(status_response), "MCP codegraph_status");
  assert(status_json.at("backend") == "empty");
  assert(status_json.at("ready") == true);

  const Json search_response = adapter.HandleRequest(
      ToolCall(4, "codegraph_search", Json{{"query", "RpcClient"}, {"limit", 5}}));
  const Json search_json =
      ParseJsonOrDie(ToolText(search_response), "MCP codegraph_search");
  assert(search_json.at("backend") == "empty");
  assert(search_json.at("query") == "RpcClient");
  assert(search_json.at("nodes").is_array());

  const Json missing_arg_response =
      adapter.HandleRequest(ToolCall(5, "codegraph_search"));
  assert(missing_arg_response.at("result").at("isError") == true);

  const Json unknown_tool_response =
      adapter.HandleRequest(ToolCall(6, "codegraph_nope"));
  assert(unknown_tool_response.at("result").at("isError") == true);

  server.Stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  assert(start_result);

  const std::uint16_t kSqlitePort = PickUnusedPort();
  const std::string fixture_path = TempDbPath("fixture");
  CreateFixtureDb(fixture_path.c_str());

  rpc::server::ServiceRegistry sqlite_registry;
  rpc::codegraph::SQLiteCodeGraphBackend sqlite_backend(fixture_path);
  assert(rpc::codegraph::RegisterCodeGraphService(sqlite_registry,
                                                  sqlite_backend));

  rpc::server::RpcServer sqlite_server(kSqlitePort, sqlite_registry, 2U, 2U);
  bool sqlite_start_result = false;
  std::thread sqlite_server_thread(
      [&]() { sqlite_start_result = sqlite_server.Start(); });

  assert(WaitServerReady(kSqlitePort, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  rpc::codegraph::McpAdapter sqlite_adapter(
      {.host = "127.0.0.1",
       .port = kSqlitePort,
       .rpc_timeout = std::chrono::milliseconds(1000)});

  const Json sqlite_status_response =
      sqlite_adapter.HandleRequest(ToolCall(7, "codegraph_status"));
  const Json sqlite_status_json =
      ParseJsonOrDie(ToolText(sqlite_status_response), "SQLite codegraph_status");
  assert(sqlite_status_json.at("backend") == "sqlite");
  assert(sqlite_status_json.at("node_count") == 3);
  assert(sqlite_status_json.at("edge_count") == 2);

  const Json sqlite_context_response =
      sqlite_adapter.HandleRequest(ToolCall(8, "codegraph_context",
                                            Json{{"symbol", "target"}}));
  const Json sqlite_context_json =
      ParseJsonOrDie(ToolText(sqlite_context_response), "SQLite codegraph_context");
  assert(sqlite_context_json.at("backend") == "sqlite");
  assert(sqlite_context_json.at("symbol").at("name") == "target");
  assert(sqlite_context_json.at("callers").size() == 1);
  assert(sqlite_context_json.at("callees").size() == 1);
  assert(sqlite_context_json.at("callers").at(0).at("name") == "caller");
  assert(sqlite_context_json.at("callees").at(0).at("name") == "callee");

  sqlite_server.Stop();
  if (sqlite_server_thread.joinable()) {
    sqlite_server_thread.join();
  }
  assert(sqlite_start_result);
  std::remove(fixture_path.c_str());

  const std::string cpp_fixture_path = TempDbPath("cpp_fixture");
  CreateCodeGraphCppFixtureDb(cpp_fixture_path.c_str());
  rpc::codegraph::SQLiteCodeGraphBackend cpp_backend(cpp_fixture_path);

  const Json cpp_status = cpp_backend.Invoke("Status", Json::object());
  assert(cpp_status.at("backend") == "sqlite");
  assert(cpp_status.at("schema") == "codegraph_cpp");
  assert(cpp_status.at("node_count") == 3);
  assert(cpp_status.at("edge_count") == 2);
  assert(cpp_status.at("file_count") == 1);

  const Json cpp_search =
      cpp_backend.Invoke("Search", Json{{"query", "target"}, {"limit", 5}});
  assert(cpp_search.at("nodes").size() == 1);
  assert(cpp_search.at("nodes").at(0).at("kind") == "function");

  const Json cpp_context =
      cpp_backend.Invoke("Context", Json{{"symbol", "target"}});
  assert(cpp_context.at("schema") == "codegraph_cpp");
  assert(cpp_context.at("symbol").at("name") == "target");
  assert(cpp_context.at("callers").size() == 1);
  assert(cpp_context.at("callees").size() == 1);
  assert(cpp_context.at("callers").at(0).at("name") == "caller");
  assert(cpp_context.at("callees").at(0).at("name") == "callee");
  std::remove(cpp_fixture_path.c_str());

  std::cout << "codegraph_mcp_rpc_test passed\n";
  return 0;
}
