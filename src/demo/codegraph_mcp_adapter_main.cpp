#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "codegraph/mcp_adapter.h"
#include "common/log.h"

namespace {

struct AdapterConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{50061};
  std::chrono::milliseconds timeout{3000};
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
            << " [--host HOST] [--port N] [--timeout-ms N]\n";
}

bool ParseArgs(int argc, char* argv[], AdapterConfig* config) {
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

    const std::string_view value = argv[++i];
    if (arg == "--host") {
      if (value.empty()) {
        std::cerr << "host must not be empty\n";
        return false;
      }
      config->host = std::string(value);
    } else if (arg == "--port") {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, &parsed) || parsed == 0 || parsed > 65535) {
        std::cerr << "invalid port\n";
        return false;
      }
      config->port = static_cast<std::uint16_t>(parsed);
    } else if (arg == "--timeout-ms") {
      std::uint64_t parsed = 0;
      if (!ParseUnsigned(value, &parsed) || parsed == 0) {
        std::cerr << "invalid timeout\n";
        return false;
      }
      config->timeout = std::chrono::milliseconds(parsed);
    } else {
      std::cerr << "unknown option: " << arg << '\n';
      return false;
    }
  }

  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  AdapterConfig config;
  if (!ParseArgs(argc, argv, &config)) {
    return 1;
  }

  // MCP speaks over stdout, so keep framework logs away from the protocol stream.
  rpc::common::SetLogLevel(rpc::common::LogLevel::kError);

  rpc::codegraph::McpAdapter adapter(
      {.host = config.host, .port = config.port, .rpc_timeout = config.timeout});
  return adapter.Run(std::cin, std::cout);
}

