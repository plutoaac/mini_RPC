#pragma once

#include <iostream>
#include <source_location>
#include <string_view>

namespace rpc::common {

enum class LogLevel {
  kInfo,
  kWarn,
  kError,
};

[[nodiscard]] inline const char* LogLevelName(LogLevel level) {
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

[[nodiscard]] inline const char* Basename(const char* full_path) {
  const char* base = full_path;
  for (const char* p = full_path; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\') {
      base = p + 1;
    }
  }
  return base;
}

inline void Log(
    LogLevel level, std::string_view message,
    const std::source_location& location = std::source_location::current()) {
  std::ostream& os = (level == LogLevel::kError)
                         ? static_cast<std::ostream&>(std::cerr)
                         : static_cast<std::ostream&>(std::cout);

  os << '[' << LogLevelName(level) << "] " << Basename(location.file_name())
     << ':' << location.line() << " " << location.function_name() << " | "
     << message << '\n';
}

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
