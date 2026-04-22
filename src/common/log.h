#pragma once

#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

namespace rpc::common {

enum class LogLevel : std::uint8_t {
  kInfo,
  kWarn,
  kError,
};

struct LoggerOptions {
  std::string file_path{"./rpc.log"};
  LogLevel min_level{LogLevel::kInfo};
  std::chrono::milliseconds flush_interval{1000};
};

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
