#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "codegraph/codegraph_rpc_service.h"
#include "codegraph/db/database.h"
#include "codegraph/graph/traverser.h"
#include "codegraph/context/context_builder.h"

namespace rpc::codegraph {

// Backend that wraps codegraph-cpp's Database/ContextBuilder/GraphTraverser
// directly, eliminating the 534-line SQL rewrite layer in sqlite_codegraph_backend.
//
// Thread safety: each Invoke() opens its own read-only Database connection
// (SQLite WAL supports concurrent readers). No global mutex needed.
//
// Performance: an LRU cache avoids repeated BFS traversals for hot queries.
class CodeGraphNativeBackend final : public CodeGraphBackend {
 public:
  explicit CodeGraphNativeBackend(std::string db_path);
  ~CodeGraphNativeBackend() override;

  CodeGraphNativeBackend(const CodeGraphNativeBackend&) = delete;
  CodeGraphNativeBackend& operator=(const CodeGraphNativeBackend&) = delete;

  [[nodiscard]] nlohmann::json Invoke(std::string_view method,
                                      const nlohmann::json& args) override;

 private:
  std::string db_path_;

  // LRU cache for expensive queries (Context, Search, Callers, Callees)
  struct CacheEntry {
    nlohmann::json result;
    std::chrono::steady_clock::time_point expires_at;
  };
  static constexpr int kCacheMaxSize = 128;
  static constexpr auto kCacheTTL = std::chrono::seconds(30);
  std::mutex cache_mu_;
  std::unordered_map<std::string, CacheEntry> cache_;

  bool cache_get(const std::string& key, nlohmann::json& out);
  void cache_put(const std::string& key, nlohmann::json result);
};

}  // namespace rpc::codegraph
