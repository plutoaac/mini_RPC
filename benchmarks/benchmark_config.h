/**
 * @file benchmark_config.h
 * @brief Benchmark configuration helper
 *
 * Provides unified benchmark configuration including:
 * - Log level control (off/error/info)
 * - Build type detection (Debug/Release)
 * - Heartbeat disable for clean benchmarks
 * - Environment info printing
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "common/log.h"

namespace rpc::benchmark {

// Forward declaration
struct BenchmarkConfig;

/**
 * Parse log level from string.
 * @param value "off", "error", or "info"
 * @return corresponding LogLevel
 */
[[nodiscard]] rpc::common::LogLevel ParseLogLevel(std::string_view value) noexcept;

/**
 * Apply log level for benchmark.
 * - off: disable all logging (fast path, zero overhead)
 * - error: only errors
 * - info: all logs
 * @param level the log level to apply
 */
void ApplyBenchmarkLogLevel(rpc::common::LogLevel level) noexcept;

/**
 * Create benchmark config with sensible defaults.
 * - log_level = kOff
 * - heartbeat disabled
 * - output to benchmarks/results
 */
[[nodiscard]] BenchmarkConfig DefaultBenchmarkConfig() noexcept;

/**
 * Print benchmark config header.
 * Shows build type, log level, heartbeat status, etc.
 */
void PrintBenchmarkHeader(const BenchmarkConfig& config,
                          std::string_view mode,
                          int connections,
                          int depth) noexcept;

/**
 * Benchmark configuration structure.
 */
struct BenchmarkConfig {
  rpc::common::LogLevel log_level{rpc::common::LogLevel::kOff};
  bool heartbeat_disabled{true};
  std::string output_dir{"benchmarks/results"};
  int payload_bytes{64};
  int total_requests{50000};

  // Build info (set at startup)
  bool is_release{false};
  const char* build_type{"Unknown"};
};

}  // namespace rpc::benchmark