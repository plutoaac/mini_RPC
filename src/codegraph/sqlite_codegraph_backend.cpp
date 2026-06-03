#include "codegraph/codegraph_rpc_service.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/rpc_error.h"

namespace rpc::codegraph {
namespace {

using Json = nlohmann::json;

struct SqliteCloser {
  void operator()(sqlite3* db) const {
    if (db != nullptr) {
      sqlite3_close(db);
    }
  }
};

using DbHandle = std::unique_ptr<sqlite3, SqliteCloser>;

struct NodeRow {
  std::string id;
  std::string kind;
  std::string name;
  std::string qualified_name;
  std::string file_path;
  std::string language;
  int start_line{0};
  int start_column{0};
  int end_line{0};
  int end_column{0};
  std::string signature;
  std::string docstring;
  bool is_static{false};
  bool is_exported{false};
};

struct EdgeRow {
  int id{0};
  std::string source;
  std::string target;
  std::string kind;
  int line{0};
  int col{0};
  std::string metadata;
};

enum class SchemaFlavor {
  kOfficialCodeGraph,
  kCodeGraphCpp,
};

rpc::server::RpcError BackendError(std::string message) {
  return rpc::server::RpcError(rpc::server::RpcStatusCode::kInternalError,
                               std::move(message));
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

const unsigned char* ColumnText(sqlite3_stmt* stmt, int col) {
  const auto* text = sqlite3_column_text(stmt, col);
  return text == nullptr ? reinterpret_cast<const unsigned char*>("") : text;
}

std::string Text(sqlite3_stmt* stmt, int col) {
  return reinterpret_cast<const char*>(ColumnText(stmt, col));
}

DbHandle OpenReadOnly(const std::string& path) {
  sqlite3* raw = nullptr;
  const int rc = sqlite3_open_v2(path.c_str(), &raw,
                                 SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                                 nullptr);
  DbHandle db(raw);
  if (rc != SQLITE_OK) {
    const std::string msg =
        raw == nullptr ? "unknown sqlite open error" : sqlite3_errmsg(raw);
    throw BackendError("failed to open CodeGraph DB '" + path + "': " + msg);
  }
  sqlite3_busy_timeout(raw, 5000);
  return db;
}

void Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err == nullptr ? "unknown sqlite error" : err;
    sqlite3_free(err);
    throw BackendError(msg);
  }
}

void CheckSqliteOk(sqlite3* db, int rc) {
  if (rc != SQLITE_OK) {
    throw BackendError(sqlite3_errmsg(db));
  }
}

bool HasColumn(sqlite3* db, std::string_view table, std::string_view column) {
  const std::string sql = "PRAGMA table_info(" + std::string(table) + ")";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
      stmt, sqlite3_finalize);

  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (Text(stmt, 1) == column) {
      return true;
    }
  }
  if (rc != SQLITE_DONE) {
    throw BackendError(sqlite3_errmsg(db));
  }
  return false;
}

SchemaFlavor DetectSchema(sqlite3* db) {
  const bool official_nodes = HasColumn(db, "nodes", "start_line") &&
                              HasColumn(db, "nodes", "start_column");
  const bool official_edges = HasColumn(db, "edges", "source") &&
                              HasColumn(db, "edges", "target");
  if (official_nodes && official_edges) {
    return SchemaFlavor::kOfficialCodeGraph;
  }

  const bool cpp_nodes = HasColumn(db, "nodes", "line") &&
                         HasColumn(db, "nodes", "col");
  const bool cpp_edges = HasColumn(db, "edges", "source_id") &&
                         HasColumn(db, "edges", "target_id");
  if (cpp_nodes && cpp_edges) {
    return SchemaFlavor::kCodeGraphCpp;
  }

  throw BackendError(
      "unsupported CodeGraph SQLite schema: expected official codegraph.db "
      "or codegraph-cpp .codegraph/index");
}

const char* SchemaName(SchemaFlavor schema) {
  switch (schema) {
    case SchemaFlavor::kOfficialCodeGraph:
      return "official_codegraph";
    case SchemaFlavor::kCodeGraphCpp:
      return "codegraph_cpp";
  }
  return "unknown";
}

std::string NodeKindCaseSql(std::string_view column) {
  return "CASE " + std::string(column) +
         " WHEN 0 THEN 'file'"
         " WHEN 1 THEN 'function'"
         " WHEN 2 THEN 'method'"
         " WHEN 3 THEN 'class'"
         " WHEN 4 THEN 'struct'"
         " WHEN 5 THEN 'enum'"
         " WHEN 6 THEN 'enum_member'"
         " WHEN 7 THEN 'variable'"
         " WHEN 8 THEN 'type_alias'"
         " WHEN 9 THEN 'namespace'"
         " WHEN 10 THEN 'import'"
         " WHEN 11 THEN 'parameter'"
         " WHEN 12 THEN 'field'"
         " ELSE 'unknown' END";
}

std::string EdgeKindCaseSql(std::string_view column) {
  return "CASE " + std::string(column) +
         " WHEN 0 THEN 'contains'"
         " WHEN 1 THEN 'calls'"
         " WHEN 2 THEN 'imports'"
         " WHEN 3 THEN 'exports'"
         " WHEN 4 THEN 'extends'"
         " WHEN 5 THEN 'implements'"
         " WHEN 6 THEN 'references'"
         " WHEN 7 THEN 'type_of'"
         " WHEN 8 THEN 'returns'"
         " WHEN 9 THEN 'overrides'"
         " ELSE 'unknown' END";
}

std::string NodeSelectSql(SchemaFlavor schema) {
  if (schema == SchemaFlavor::kOfficialCodeGraph) {
    return "SELECT id, kind, name, qualified_name, file_path, language, "
           "start_line, start_column, end_line, end_column, signature, "
           "docstring, is_static, is_exported FROM nodes ";
  }

  return "SELECT CAST(id AS TEXT), " + NodeKindCaseSql("kind") +
         ", name, COALESCE(qualified_name, name), file_path, "
         "COALESCE(language, ''), line, col, end_line, end_col, "
         "COALESCE(signature, ''), COALESCE(docstring, ''), "
         "is_static, is_exported FROM nodes ";
}

int64_t Count(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw BackendError(sqlite3_errmsg(db));
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
      stmt, sqlite3_finalize);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    throw BackendError(sqlite3_errmsg(db));
  }
  return sqlite3_column_int64(stmt, 0);
}

NodeRow ReadNode(sqlite3_stmt* stmt) {
  NodeRow node;
  node.id = Text(stmt, 0);
  node.kind = Text(stmt, 1);
  node.name = Text(stmt, 2);
  node.qualified_name = Text(stmt, 3);
  node.file_path = Text(stmt, 4);
  node.language = Text(stmt, 5);
  node.start_line = sqlite3_column_int(stmt, 6);
  node.start_column = sqlite3_column_int(stmt, 7);
  node.end_line = sqlite3_column_int(stmt, 8);
  node.end_column = sqlite3_column_int(stmt, 9);
  node.signature = Text(stmt, 10);
  node.docstring = Text(stmt, 11);
  node.is_static = sqlite3_column_int(stmt, 12) != 0;
  node.is_exported = sqlite3_column_int(stmt, 13) != 0;
  return node;
}

Json NodeToJson(const NodeRow& node) {
  return Json{{"id", node.id},
              {"kind", node.kind},
              {"name", node.name},
              {"qualified_name", node.qualified_name},
              {"file", node.file_path},
              {"language", node.language},
              {"line", node.start_line},
              {"col", node.start_column},
              {"end_line", node.end_line},
              {"end_col", node.end_column},
              {"signature", node.signature},
              {"docstring", node.docstring},
              {"is_static", node.is_static},
              {"is_exported", node.is_exported}};
}

Json EdgeToJson(const EdgeRow& edge) {
  return Json{{"id", edge.id},
              {"source_id", edge.source},
              {"target_id", edge.target},
              {"kind", edge.kind},
              {"line", edge.line},
              {"col", edge.col},
              {"metadata", edge.metadata}};
}

std::vector<NodeRow> QueryNodes(sqlite3* db, const char* sql,
                                const std::vector<std::string>& text_params,
                                int limit) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw BackendError(sqlite3_errmsg(db));
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
      stmt, sqlite3_finalize);

  int index = 1;
  for (const auto& param : text_params) {
    CheckSqliteOk(
        db, sqlite3_bind_text(stmt, index++, param.c_str(), -1, SQLITE_TRANSIENT));
  }
  CheckSqliteOk(db, sqlite3_bind_int(stmt, index, limit));

  std::vector<NodeRow> rows;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    rows.push_back(ReadNode(stmt));
  }
  if (rc != SQLITE_DONE) {
    throw BackendError(sqlite3_errmsg(db));
  }
  return rows;
}

std::vector<NodeRow> FindNodes(sqlite3* db, const std::string& symbol,
                               SchemaFlavor schema, int limit) {
  const std::string select = NodeSelectSql(schema);
  std::string exact_sql =
      select +
      "WHERE name=? OR qualified_name=? "
      "ORDER BY CASE WHEN name=? THEN 0 WHEN qualified_name=? THEN 1 ELSE 2 END "
      "LIMIT ?";
  auto exact = QueryNodes(db, exact_sql.c_str(),
                          {symbol, symbol, symbol, symbol}, limit);
  if (static_cast<int>(exact.size()) >= limit) {
    return exact;
  }

  const int remaining = limit - static_cast<int>(exact.size());
  const std::string pattern = "%" + symbol + "%";
  std::string like_sql =
      select +
      "WHERE (name LIKE ? OR qualified_name LIKE ?) "
      "AND name<>? AND qualified_name<>? LIMIT ?";
  auto fuzzy = QueryNodes(db, like_sql.c_str(),
                          {pattern, pattern, symbol, symbol}, remaining);
  exact.insert(exact.end(), fuzzy.begin(), fuzzy.end());
  return exact;
}

std::vector<NodeRow> SearchNodes(sqlite3* db, const std::string& query,
                                 SchemaFlavor schema, int limit) {
  std::string sql = NodeSelectSql(schema) +
                    "WHERE lower(name) LIKE lower(?) OR "
                    "lower(qualified_name) LIKE lower(?) "
                    "ORDER BY CASE WHEN name=? THEN 0 "
                    "WHEN qualified_name=? THEN 1 ELSE 2 END, "
                    "file_path, ";
  sql += schema == SchemaFlavor::kOfficialCodeGraph ? "start_line" : "line";
  sql += " LIMIT ?";

  const std::string pattern = "%" + query + "%";
  return QueryNodes(db, sql.c_str(), {pattern, pattern, query, query}, limit);
}

std::vector<EdgeRow> QueryEdges(sqlite3* db, SchemaFlavor schema,
                                const std::string& node_id, std::string_view direction,
                                std::string_view kind, int limit) {
  const bool outgoing = direction == "out";
  std::string sql;
  if (schema == SchemaFlavor::kOfficialCodeGraph) {
    sql = outgoing
              ? "SELECT id, source, target, kind, line, col, metadata "
                "FROM edges WHERE source=? AND kind=? LIMIT ?"
              : "SELECT id, source, target, kind, line, col, metadata "
                "FROM edges WHERE target=? AND kind=? LIMIT ?";
  } else {
    sql = outgoing ? "SELECT id, CAST(source_id AS TEXT), "
                     "CAST(target_id AS TEXT), "
                   : "SELECT id, CAST(source_id AS TEXT), "
                     "CAST(target_id AS TEXT), ";
    sql += EdgeKindCaseSql("kind");
    sql += outgoing
               ? ", line, col, COALESCE(metadata, '') FROM edges "
                 "WHERE source_id=? AND kind=? LIMIT ?"
               : ", line, col, COALESCE(metadata, '') FROM edges "
                 "WHERE target_id=? AND kind=? LIMIT ?";
  }

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw BackendError(sqlite3_errmsg(db));
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
      stmt, sqlite3_finalize);

  CheckSqliteOk(
      db, sqlite3_bind_text(stmt, 1, node_id.c_str(), -1, SQLITE_TRANSIENT));
  const std::string kind_string(kind);
  if (schema == SchemaFlavor::kOfficialCodeGraph) {
    CheckSqliteOk(
        db, sqlite3_bind_text(stmt, 2, kind_string.c_str(), -1, SQLITE_TRANSIENT));
  } else {
    CheckSqliteOk(db, sqlite3_bind_int(stmt, 2, kind == "calls" ? 1 : -1));
  }
  CheckSqliteOk(db, sqlite3_bind_int(stmt, 3, limit));

  std::vector<EdgeRow> edges;
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    EdgeRow edge;
    edge.id = sqlite3_column_int(stmt, 0);
    edge.source = Text(stmt, 1);
    edge.target = Text(stmt, 2);
    edge.kind = Text(stmt, 3);
    edge.line = sqlite3_column_int(stmt, 4);
    edge.col = sqlite3_column_int(stmt, 5);
    edge.metadata = Text(stmt, 6);
    edges.push_back(edge);
  }
  if (rc != SQLITE_DONE) {
    throw BackendError(sqlite3_errmsg(db));
  }
  return edges;
}

std::vector<NodeRow> GetNodesByIds(sqlite3* db, SchemaFlavor schema,
                                   const std::vector<std::string>& ids) {
  std::vector<NodeRow> nodes;
  std::string sql = NodeSelectSql(schema);
  sql += schema == SchemaFlavor::kOfficialCodeGraph
             ? "WHERE id=? LIMIT ?"
             : "WHERE CAST(id AS TEXT)=? LIMIT ?";
  for (const auto& id : ids) {
    auto rows = QueryNodes(db, sql.c_str(), {id}, 1);
    nodes.insert(nodes.end(), rows.begin(), rows.end());
  }
  return nodes;
}

Json Traversal(sqlite3* db, SchemaFlavor schema, const NodeRow& start,
               std::string_view direction, int max_depth, int limit) {
  Json result;
  result["nodes"] = Json::array();
  result["edges"] = Json::array();

  std::vector<std::string> frontier{start.id};
  std::unordered_set<std::string> visited{start.id};
  int emitted = 0;

  for (int depth = 0; depth < max_depth && !frontier.empty() && emitted < limit;
       ++depth) {
    std::vector<std::string> next;
    for (const auto& node_id : frontier) {
      auto edges = QueryEdges(db, schema, node_id, direction, "calls", limit);
      for (const auto& edge : edges) {
        const std::string adjacent = direction == "out" ? edge.target : edge.source;
        if (visited.insert(adjacent).second) {
          next.push_back(adjacent);
          result["edges"].push_back(EdgeToJson(edge));
          emitted++;
          if (emitted >= limit) break;
        }
      }
      if (emitted >= limit) break;
    }

    auto nodes = GetNodesByIds(db, schema, next);
    for (const auto& node : nodes) {
      result["nodes"].push_back(NodeToJson(node));
    }
    frontier = std::move(next);
  }

  return result;
}

Json Files(sqlite3* db, int limit) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT path, language FROM files ORDER BY path LIMIT ?";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    throw BackendError(sqlite3_errmsg(db));
  }
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(
      stmt, sqlite3_finalize);
  CheckSqliteOk(db, sqlite3_bind_int(stmt, 1, limit));

  Json files = Json::array();
  int rc = SQLITE_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    files.push_back({{"path", Text(stmt, 0)}, {"language", Text(stmt, 1)}});
  }
  if (rc != SQLITE_DONE) {
    throw BackendError(sqlite3_errmsg(db));
  }
  return files;
}

}  // namespace

SQLiteCodeGraphBackend::SQLiteCodeGraphBackend(std::string db_path)
    : db_path_(std::move(db_path)) {}

nlohmann::json SQLiteCodeGraphBackend::Invoke(std::string_view method,
                                              const nlohmann::json& args) {
  auto db = OpenReadOnly(db_path_);
  Exec(db.get(), "PRAGMA query_only=ON");
  const SchemaFlavor schema = DetectSchema(db.get());

  if (method == "Status") {
    return Json{{"service", "CodeGraph"},
                {"backend", "sqlite"},
                {"schema", SchemaName(schema)},
                {"db_path", db_path_},
                {"ready", true},
                {"transport", "mini_rpc"},
                {"node_count", Count(db.get(), "SELECT COUNT(*) FROM nodes")},
                {"edge_count", Count(db.get(), "SELECT COUNT(*) FROM edges")},
                {"file_count", Count(db.get(), "SELECT COUNT(*) FROM files")}};
  }

  if (method == "Files") {
    const int limit = OptionalInt(args, method, "limit", 200, 1, 10000);
    return Json{{"backend", "sqlite"},
                {"schema", SchemaName(schema)},
                {"files", Files(db.get(), limit)}};
  }

  if (method == "Search") {
    const std::string query = RequiredString(args, method, "query");
    const int limit = OptionalInt(args, method, "limit", 20, 1, 200);
    Json nodes = Json::array();
    for (const auto& node : SearchNodes(db.get(), query, schema, limit)) {
      nodes.push_back(NodeToJson(node));
    }
    return Json{{"backend", "sqlite"},
                {"schema", SchemaName(schema)},
                {"query", query},
                {"limit", limit},
                {"nodes", std::move(nodes)}};
  }

  if (method == "Node") {
    const std::string symbol = RequiredString(args, method, "symbol");
    auto nodes = FindNodes(db.get(), symbol, schema, 1);
    if (nodes.empty()) {
      return Json{{"error", "Symbol not found: " + symbol}};
    }
    return Json{{"backend", "sqlite"},
                {"schema", SchemaName(schema)},
                {"node", NodeToJson(nodes[0])}};
  }

  if (method == "Callers" || method == "Callees" || method == "Impact" ||
      method == "Context") {
    const std::string symbol = RequiredString(args, method, "symbol");
    auto nodes = FindNodes(db.get(), symbol, schema, 1);
    if (nodes.empty()) {
      return Json{{"error", "Symbol not found: " + symbol}};
    }

    const int default_depth = method == "Impact" ? 5 : 3;
    const int max_depth = OptionalInt(args, method, "max_depth", default_depth, 1, 20);
    const int limit = OptionalInt(args, method, "limit", 50, 1, 500);
    const NodeRow& start = nodes[0];

    if (method == "Callers") {
      Json result = Traversal(db.get(), schema, start, "in", max_depth, limit);
      result["backend"] = "sqlite";
      result["schema"] = SchemaName(schema);
      result["symbol"] = NodeToJson(start);
      return result;
    }
    if (method == "Callees") {
      Json result = Traversal(db.get(), schema, start, "out", max_depth, limit);
      result["backend"] = "sqlite";
      result["schema"] = SchemaName(schema);
      result["symbol"] = NodeToJson(start);
      return result;
    }
    if (method == "Impact") {
      Json result = Traversal(db.get(), schema, start, "in", max_depth, limit);
      result["backend"] = "sqlite";
      result["schema"] = SchemaName(schema);
      result["symbol"] = NodeToJson(start);
      return result;
    }

    Json callers = Traversal(db.get(), schema, start, "in", 2, limit);
    Json callees = Traversal(db.get(), schema, start, "out", 2, limit);
    Json edges = Json::array();
    for (const auto& edge : callers["edges"]) edges.push_back(edge);
    for (const auto& edge : callees["edges"]) edges.push_back(edge);
    return Json{{"backend", "sqlite"},
                {"schema", SchemaName(schema)},
                {"symbol", NodeToJson(start)},
                {"callers", callers["nodes"]},
                {"callees", callees["nodes"]},
                {"edges", std::move(edges)}};
  }

  throw rpc::server::RpcError(rpc::server::RpcStatusCode::kMethodNotFound,
                              "unknown CodeGraph method: " +
                                  std::string(method));
}

}  // namespace rpc::codegraph
