#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>

namespace rpc::common {

enum class LogLevel : std::uint8_t {
  kInfo,
  kWarn,
  kError,
};

enum class DropPolicy : std::uint8_t {
  DropNewest,
  DropOldest,
};

struct LoggerOptions {
  std::filesystem::path log_file_path{"./rpc.log"};
  std::size_t queue_capacity{1u << 16};
  std::size_t max_batch_size{512};
  std::chrono::milliseconds flush_interval{500};
  std::uint32_t spin_before_sleep{128};
  std::chrono::microseconds idle_sleep{150};
  DropPolicy drop_policy{DropPolicy::DropNewest};
  LogLevel min_level{LogLevel::kInfo};
  // Threshold for choosing inline vs heap message storage.
  std::size_t inline_message_threshold{112};

  // Compatibility field for older call sites.
  std::string file_path{};
};

// Preset focused on peak throughput and lower drop rate under bursts.
[[nodiscard]] LoggerOptions ThroughputFirstLoggerOptions();

// Preset focused on balanced CPU usage, drop rate, and shutdown latency.
[[nodiscard]] LoggerOptions BalancedLoggerOptions();

struct LoggerRuntimeStats {
  std::uint64_t submit_calls{0};
  std::uint64_t filtered_by_level{0};
  std::uint64_t enqueued{0};
  std::uint64_t consumed{0};
  std::uint64_t dropped{0};
};

[[nodiscard]] constexpr const char* LogLevelName(LogLevel level) noexcept {
  switch (level) {
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarn:
      return "WARN";
    case LogLevel::kError:
      return "ERROR";
    default:
      return "UNKNOWN";
  }
}

[[nodiscard]] constexpr const char* Basename(const char* full_path) noexcept {
  const char* base = full_path;
  for (const char* p = full_path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

void InitLogger(const LoggerOptions& options = LoggerOptions{});
void SetLogFile(std::string_view path);
void SetLogLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel GetLogLevel() noexcept;
[[nodiscard]] LoggerRuntimeStats GetLoggerRuntimeStats() noexcept;
void FlushLogger() noexcept;
void ShutdownLogger() noexcept;
void StopLogger() noexcept;

void Log(
    LogLevel level, std::string_view message,
    const std::source_location& location = std::source_location::current())
    noexcept;

inline void LogInfo(
    std::string_view message,
    const std::source_location& location = std::source_location::current()) {
  Log(LogLevel::kInfo, message, location);
}

inline void LogWarn(
    std::string_view message,
    const std::source_location& location = std::source_location::current()) {
  Log(LogLevel::kWarn, message, location);
}

inline void LogError(
    std::string_view message,
    const std::source_location& location = std::source_location::current()) {
  Log(LogLevel::kError, message, location);
}

}  // namespace rpc::common
