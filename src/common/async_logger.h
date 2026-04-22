#pragma once

#include "common/log.h"

#include <source_location>
#include <string_view>

namespace rpc::common::detail {

void InitAsyncLogger(const LoggerOptions& options);
void SetAsyncLoggerFile(std::string_view path);
void SetAsyncLoggerLevel(LogLevel level) noexcept;
[[nodiscard]] LogLevel GetAsyncLoggerLevel() noexcept;
[[nodiscard]] LoggerRuntimeStats GetAsyncLoggerRuntimeStats() noexcept;
void FlushAsyncLogger() noexcept;
void ShutdownAsyncLogger() noexcept;
void SubmitAsyncLog(LogLevel level, std::string_view message,
                    const std::source_location& location) noexcept;

}  // namespace rpc::common::detail
