/**
 * @file benchmark_config.cpp
 * @brief Benchmark configuration helper implementation
 */

#include "benchmark_config.h"

#include <iostream>

namespace rpc::benchmark {

using namespace rpc::common;

LogLevel ParseLogLevel(std::string_view value) noexcept {
  if (value == "off") {
    return LogLevel::kOff;
  }
  if (value == "error") {
    return LogLevel::kError;
  }
  if (value == "info") {
    return LogLevel::kInfo;
  }
  // Default to off for benchmark cleanliness
  return LogLevel::kOff;
}

void ApplyBenchmarkLogLevel(LogLevel level) noexcept {
  rpc::common::SetLogLevel(level);
}

BenchmarkConfig DefaultBenchmarkConfig() noexcept {
  BenchmarkConfig config;
  config.log_level = LogLevel::kOff;
  config.heartbeat_disabled = true;
  config.output_dir = "benchmarks/results";
  config.payload_bytes = 64;
  config.total_requests = 50000;

#if defined(NDEBUG)
  config.is_release = true;
  config.build_type = "Release";
#else
  config.is_release = false;
  config.build_type = "Debug";
#endif

  return config;
}

void PrintBenchmarkHeader(const BenchmarkConfig& config,
                          std::string_view mode,
                          int connections,
                          int depth) noexcept {
  std::cout << "\n=== Benchmark Config ===\n";
  std::cout << "build_type=" << config.build_type << "\n";
  std::cout << "log_level=" << LogLevelName(config.log_level) << "\n";
  std::cout << "heartbeat=" << (config.heartbeat_disabled ? "disabled" : "enabled")
            << "\n";
  std::cout << "payload_bytes=" << config.payload_bytes << "\n";
  std::cout << "requests=" << config.total_requests << "\n";
  std::cout << "mode=" << mode << "\n";
  std::cout << "connections=" << connections << "\n";
  std::cout << "depth=" << depth << "\n";
  std::cout << "========================\n\n";
}

}  // namespace rpc::benchmark