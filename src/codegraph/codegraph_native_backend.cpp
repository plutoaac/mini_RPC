#include "codegraph/codegraph_native_backend.h"

#include "common/rpc_error.h"

#include <string>
#include <string_view>
#include <utility>

namespace rpc::codegraph {
namespace {

using Json = nlohmann::json;

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
  if (it == args.end() || it->is_null()) return default_value;
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

// Normalize codegraph-cpp JSON format to match what MCP adapter expects
Json NormalizeNodeJson(Json node) {
  if (node.contains("file") && !node.contains("file_path")) {
    node["file_path"] = node["file"];
    node.erase("file");
  }
  if (!node.contains("signature")) node["signature"] = "";
  return node;
}

Json NormalizeEdgeJson(Json edge) {
  if (edge.contains("src") && !edge.contains("source_id")) {
    edge["source_id"] = edge["src"];
    edge.erase("src");
  }
  if (edge.contains("dst") && !edge.contains("target_id")) {
    edge["target_id"] = edge["dst"];
    edge.erase("dst");
  }
  if (!edge.contains("line")) edge["line"] = 0;
  return edge;
}

Json NormalizeNodesArray(const Json& nodes) {
  Json result = Json::array();
  for (const auto& node : nodes) result.push_back(NormalizeNodeJson(node));
  return result;
}

Json NormalizeEdgesArray(const Json& edges) {
  Json result = Json::array();
  for (const auto& edge : edges) result.push_back(NormalizeEdgeJson(edge));
  return result;
}

// Build a cache key from method + args
std::string MakeCacheKey(std::string_view method, const Json& args) {
  return std::string(method) + ":" + args.dump();
}

}  // namespace

CodeGraphNativeBackend::CodeGraphNativeBackend(std::string db_path)
    : db_path_(std::move(db_path)) {}

CodeGraphNativeBackend::~CodeGraphNativeBackend() = default;

bool CodeGraphNativeBackend::cache_get(const std::string& key,
                                       nlohmann::json& out) {
  std::lock_guard<std::mutex> lock(cache_mu_);
  auto it = cache_.find(key);
  if (it == cache_.end()) return false;
  if (std::chrono::steady_clock::now() > it->second.expires_at) {
    cache_.erase(it);
    return false;
  }
  out = it->second.result;
  return true;
}

void CodeGraphNativeBackend::cache_put(const std::string& key,
                                       nlohmann::json result) {
  std::lock_guard<std::mutex> lock(cache_mu_);
  if (static_cast<int>(cache_.size()) >= kCacheMaxSize) {
    // Evict oldest entry (simple: just clear half)
    auto mid = cache_.begin();
    std::advance(mid, cache_.size() / 2);
    cache_.erase(cache_.begin(), mid);
  }
  cache_[key] = {std::move(result),
                 std::chrono::steady_clock::now() + kCacheTTL};
}

// 每次调用打开独立的 Database 连接（SQLite WAL 模式支持并发读）。
// 不需要全局 mutex——每个连接独立，读-读不阻塞。
//
// 流程：
//   1. 检查 LRU 缓存（热门查询避免重复 BFS 遍历）
//   2. 缓存未命中：打开连接 → 执行查询 → 结果写入缓存
//   3. 返回 JSON 结果
nlohmann::json CodeGraphNativeBackend::Invoke(std::string_view method,
                                              const nlohmann::json& args) {
  // ── Status: 轻量查询，不缓存（每次返回最新计数）──
  if (method == "Status") {
    ::codegraph::Database db(db_path_);
    ::codegraph::GraphTraverser traverser(db);
    auto status = ::codegraph::ContextBuilder(db, traverser).get_status();
    status["backend"] = "native";
    status["db_path"] = db_path_;
    status["ready"] = true;
    status["transport"] = "mini_rpc";
    return status;
  }

  // ── Files: cheap, no cache ──
  if (method == "Files") {
    ::codegraph::Database db(db_path_);
    const int limit = OptionalInt(args, method, "limit", 200, 1, 10000);
    auto files = db.get_all_files();
    Json result = Json::array();
    int count = 0;
    for (const auto& f : files) {
      if (count >= limit) break;
      result.push_back(Json{{"path", f.path},
                            {"language", f.language},
                            {"mtime", f.mtime},
                            {"size", f.size}});
      count++;
    }
    return Json{{"backend", "native"}, {"files", std::move(result)}};
  }

  // ── Expensive queries: check cache first ──
  std::string cache_key = MakeCacheKey(method, args);
  Json cached;
  if (cache_get(cache_key, cached)) return cached;

  // Open per-request connection (SQLite WAL: concurrent readers OK)
  ::codegraph::Database db(db_path_);
  ::codegraph::GraphTraverser traverser(db);
  ::codegraph::ContextBuilder context(db, traverser);

  Json result;

  if (method == "Search") {
    const std::string query = RequiredString(args, method, "query");
    const int limit = OptionalInt(args, method, "limit", 20, 1, 200);
    auto nodes_array = context.search_symbols(query, limit);
    result = Json{{"backend", "native"},
                  {"query", query},
                  {"limit", limit},
                  {"nodes", NormalizeNodesArray(nodes_array)}};

  } else if (method == "Node") {
    const std::string symbol = RequiredString(args, method, "symbol");
    auto nodes_array = context.search_symbols(symbol, 1);
    if (nodes_array.is_array() && !nodes_array.empty()) {
      result = Json{{"backend", "native"},
                    {"node", NormalizeNodeJson(nodes_array[0])}};
    } else {
      result = Json{{"error", "Symbol not found: " + symbol}};
    }

  } else if (method == "Context") {
    const std::string symbol = RequiredString(args, method, "symbol");
    const int limit = OptionalInt(args, method, "limit", 10, 1, 200);
    const int max_depth = OptionalInt(args, method, "max_depth", 3, 1, 20);
    result = context.build_context(symbol, limit, max_depth);
    result["backend"] = "native";
    if (result.contains("callers"))
      result["callers"] = NormalizeNodesArray(result["callers"]);
    if (result.contains("callees"))
      result["callees"] = NormalizeNodesArray(result["callees"]);
    if (result.contains("edges"))
      result["edges"] = NormalizeEdgesArray(result["edges"]);
    if (result.contains("symbol"))
      result["symbol"] = NormalizeNodeJson(result["symbol"]);

  } else if (method == "Callers") {
    const std::string symbol = RequiredString(args, method, "symbol");
    const int max_depth = OptionalInt(args, method, "max_depth", 3, 1, 20);
    result = context.get_callers(symbol, max_depth);
    result["backend"] = "native";
    if (result.contains("nodes"))
      result["nodes"] = NormalizeNodesArray(result["nodes"]);
    if (result.contains("edges"))
      result["edges"] = NormalizeEdgesArray(result["edges"]);

  } else if (method == "Callees") {
    const std::string symbol = RequiredString(args, method, "symbol");
    const int max_depth = OptionalInt(args, method, "max_depth", 3, 1, 20);
    result = context.get_callees(symbol, max_depth);
    result["backend"] = "native";
    if (result.contains("nodes"))
      result["nodes"] = NormalizeNodesArray(result["nodes"]);
    if (result.contains("edges"))
      result["edges"] = NormalizeEdgesArray(result["edges"]);

  } else if (method == "Impact") {
    const std::string symbol = RequiredString(args, method, "symbol");
    const int max_depth = OptionalInt(args, method, "max_depth", 5, 1, 20);
    result = context.get_impact(symbol, max_depth);
    result["backend"] = "native";
    if (result.contains("nodes"))
      result["nodes"] = NormalizeNodesArray(result["nodes"]);
    if (result.contains("edges"))
      result["edges"] = NormalizeEdgesArray(result["edges"]);

  } else {
    throw rpc::server::RpcError(rpc::server::RpcStatusCode::kMethodNotFound,
                                "unknown CodeGraph method: " +
                                    std::string(method));
  }

  cache_put(cache_key, result);
  return result;
}

}  // namespace rpc::codegraph
