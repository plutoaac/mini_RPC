#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#ifdef HAS_CODEGRAPH_CPP
#include "codegraph/codegraph_native_backend.h"
#endif
#include "codegraph/codegraph_rpc_service.h"
#include "common/log.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

namespace {

enum class BackendKind {
  kSqlite,
  kNative,
};

struct ServerConfig {
  std::uint16_t port{50061};
  std::size_t workers{2};
  std::size_t business_threads{4};
  std::string db_path;
  BackendKind backend{BackendKind::kSqlite};
};

bool ParseUnsigned(std::string_view text, std::uint64_t* out) {
  if (out == nullptr || text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc{} || ptr != last) {
    return false;
  }
  *out = value;
  return true;
}

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [--port N] [--workers N] [--business-threads N]"
               " [--db PATH] [--backend sqlite|native]\n";
}

bool ParseArgs(int argc, char* argv[], ServerConfig* config) {
  if (config == nullptr) {
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return false;
    }

    if (i + 1 >= argc) {
      std::cerr << "missing value for " << arg << '\n';
      return false;
    }

    const std::string_view value_text = argv[++i];

    if (arg == "--port") {
      std::uint64_t value = 0;
      if (!ParseUnsigned(value_text, &value)) {
        std::cerr << "invalid numeric value for " << arg << '\n';
        return false;
      }
      if (value == 0 || value > 65535) {
        std::cerr << "port out of range\n";
        return false;
      }
      config->port = static_cast<std::uint16_t>(value);
    } else if (arg == "--workers") {
      std::uint64_t value = 0;
      if (!ParseUnsigned(value_text, &value)) {
        std::cerr << "invalid numeric value for " << arg << '\n';
        return false;
      }
      if (value == 0) {
        std::cerr << "workers must be greater than zero\n";
        return false;
      }
      config->workers = static_cast<std::size_t>(value);
    } else if (arg == "--business-threads") {
      std::uint64_t value = 0;
      if (!ParseUnsigned(value_text, &value)) {
        std::cerr << "invalid numeric value for " << arg << '\n';
        return false;
      }
      config->business_threads = static_cast<std::size_t>(value);
    } else if (arg == "--db") {
      if (value_text.empty()) {
        std::cerr << "db path must not be empty\n";
        return false;
      }
      config->db_path = std::string(value_text);
    } else if (arg == "--backend") {
      if (value_text == "sqlite") {
        config->backend = BackendKind::kSqlite;
      } else if (value_text == "native") {
        config->backend = BackendKind::kNative;
      } else {
        std::cerr << "backend must be sqlite or native\n";
        return false;
      }
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
  }

  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  ServerConfig config;
  if (!ParseArgs(argc, argv, &config)) {
    return 1;
  }

  rpc::common::SetLogLevel(rpc::common::LogLevel::kInfo);

  rpc::server::ServiceRegistry registry;
  std::unique_ptr<rpc::codegraph::CodeGraphBackend> backend;
  if (config.db_path.empty()) {
    backend = std::make_unique<rpc::codegraph::EmptyCodeGraphBackend>();
  } else {
    if (config.backend == BackendKind::kNative) {
#ifdef HAS_CODEGRAPH_CPP
      backend =
          std::make_unique<rpc::codegraph::CodeGraphNativeBackend>(config.db_path);
#else
      rpc::common::LogError(
          "native CodeGraph backend is not available in this build");
      return 1;
#endif
    } else {
      backend = std::make_unique<rpc::codegraph::SQLiteCodeGraphBackend>(
          config.db_path);
    }
    try {
      (void)backend->Invoke("Status", nlohmann::json::object());
    } catch (const std::exception& ex) {
      rpc::common::LogError(std::string("invalid CodeGraph DB: ") + ex.what());
      return 1;
    }
  }

  if (!rpc::codegraph::RegisterCodeGraphService(registry, *backend)) {
    rpc::common::LogError("failed to register CodeGraph RPC service");
    return 1;
  }

  rpc::server::RpcServer server(config.port, registry, config.workers,
                                config.business_threads);
  std::cout << "CodeGraph RPC server listening on 0.0.0.0:" << config.port
            << " workers=" << config.workers
            << " business_threads=" << config.business_threads
            << " backend="
            << (config.db_path.empty()
                    ? "empty"
                    : (config.backend == BackendKind::kNative ? "native"
                                                              : "sqlite"))
            << " db=" << (config.db_path.empty() ? "-" : config.db_path)
            << '\n';
  std::cout.flush();

  if (!server.Start()) {
    rpc::common::LogError("CodeGraph RPC server failed to start");
    return 1;
  }

  return 0;
}
