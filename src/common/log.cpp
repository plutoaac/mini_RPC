#include "common/log.h"

#include "common/async_logger.h"

namespace rpc::common {
void InitLogger(const LoggerOptions& options) { detail::InitAsyncLogger(options); }

void SetLogFile(std::string_view path) { detail::SetAsyncLoggerFile(path); }

void SetLogLevel(LogLevel level) noexcept { detail::SetAsyncLoggerLevel(level); }

LogLevel GetLogLevel() noexcept { return detail::GetAsyncLoggerLevel(); }

LoggerRuntimeStats GetLoggerRuntimeStats() noexcept {
  return detail::GetAsyncLoggerRuntimeStats();
}

void FlushLogger() noexcept { detail::FlushAsyncLogger(); }

void ShutdownLogger() noexcept { detail::ShutdownAsyncLogger(); }

void StopLogger() noexcept { ShutdownLogger(); }

void Log(LogLevel level, std::string_view message,
         const std::source_location& location) noexcept {
  detail::SubmitAsyncLog(level, message, location);
}

}  // namespace rpc::common