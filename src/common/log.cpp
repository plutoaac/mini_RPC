#include "common/log.h"

#include "common/async_logger.h"

namespace rpc::common {
LoggerOptions ThroughputFirstLoggerOptions() {
  LoggerOptions options;
  options.queue_capacity = 1u << 18;
  options.max_batch_size = 1024;
  options.flush_interval = std::chrono::milliseconds(500);
  options.spin_before_sleep = 256;
  options.idle_sleep = std::chrono::microseconds(50);
  options.drop_policy = DropPolicy::DropNewest;
  options.inline_message_threshold = 112;
  return options;
}

LoggerOptions BalancedLoggerOptions() {
  LoggerOptions options;
  options.queue_capacity = 1u << 16;
  options.max_batch_size = 512;
  options.flush_interval = std::chrono::milliseconds(500);
  options.spin_before_sleep = 128;
  options.idle_sleep = std::chrono::microseconds(150);
  options.drop_policy = DropPolicy::DropNewest;
  options.inline_message_threshold = 112;
  return options;
}

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