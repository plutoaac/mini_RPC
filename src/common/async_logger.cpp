#include "common/async_logger.h"

#include "common/mpsc_ring_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace rpc::common::detail {
namespace {

constexpr std::size_t kQueueCapacity = 1u << 18;
constexpr std::size_t kBatchSize = 1024;
constexpr std::size_t kSpinBeforeSleep = 256;
constexpr std::chrono::microseconds kIdleSleep{50};
constexpr int kPushRetryLimit = 2;
constexpr std::chrono::milliseconds kDroppedReportInterval{500};
constexpr std::chrono::milliseconds kDefaultFlushInterval{1000};
constexpr std::string_view kDefaultLogFile = "./rpc.log";

// Used to guarantee we only register one atexit hook.
constinit std::atomic<bool> g_atexit_registered{false};

[[nodiscard]] std::uint64_t CurrentThreadId() noexcept {
  static thread_local const std::uint64_t cached_tid =
      static_cast<std::uint64_t>(
          std::hash<std::thread::id>{}(std::this_thread::get_id()));
  return cached_tid;
}

[[nodiscard]] std::string TrimPath(std::string_view raw) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

  // Find first non-space from left
  auto left_it = raw.begin();
  while (left_it != raw.end() && is_space(static_cast<unsigned char>(*left_it))) {
    ++left_it;
  }
  if (left_it == raw.end()) {
    return {};
  }

  // Find first non-space from right
  auto right_it = raw.end();
  while (right_it != raw.begin()) {
    --right_it;
    if (!is_space(static_cast<unsigned char>(*right_it))) {
      ++right_it;
      break;
    }
  }

  return std::string(left_it, right_it);
}

[[nodiscard]] constexpr std::uint8_t LevelValue(LogLevel level) noexcept {
  return static_cast<std::uint8_t>(level);
}

struct LogEntry {
  std::chrono::system_clock::time_point timestamp{
      std::chrono::system_clock::now()};
  LogLevel level{LogLevel::kInfo};
  std::uint64_t tid{0};
  const char* file_name{"unknown"};
  std::uint_least32_t line{0};
  const char* function_name{"unknown"};
  std::string message;
};

class AsyncLoggerEngine final {
 public:
  AsyncLoggerEngine() {
    last_drop_report_time_ = std::chrono::steady_clock::now();
    StartWorkerIfNeeded();
    RegisterAtExit();
  }

  ~AsyncLoggerEngine() { Shutdown(); }

  void Init(const LoggerOptions& options) {
    SetLogLevel(options.min_level);

    const auto flush_ms =
        std::max<std::int64_t>(1, options.flush_interval.count());
    flush_interval_ms_.store(flush_ms, std::memory_order_release);

    if (!options.file_path.empty()) {
      SetLogFile(options.file_path);
    }

    StartWorkerIfNeeded();
  }

  void SetLogFile(std::string_view path) {
    const std::string normalized = TrimPath(path);
    if (normalized.empty()) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(path_mu_);
      pending_file_path_ = normalized;
    }
    reopen_requested_.store(true, std::memory_order_release);
    StartWorkerIfNeeded();
  }

  void SetLogLevel(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_release);
  }

  [[nodiscard]] LogLevel GetLogLevel() const noexcept {
    return min_level_.load(std::memory_order_acquire);
  }

  [[nodiscard]] LoggerRuntimeStats GetRuntimeStats() const noexcept {
    LoggerRuntimeStats stats;
    stats.submit_calls = submit_calls_.load(std::memory_order_acquire);
    stats.filtered_by_level =
        filtered_by_level_.load(std::memory_order_acquire);
    stats.enqueued = enqueued_.load(std::memory_order_acquire);
    stats.consumed = consumed_.load(std::memory_order_acquire);
    stats.dropped = dropped_.load(std::memory_order_acquire);
    return stats;
  }

  void Flush() noexcept { flush_requested_.store(true, std::memory_order_release); }

  void Submit(LogLevel level, std::string_view message,
              const std::source_location& location) noexcept {
    submit_calls_.fetch_add(1, std::memory_order_relaxed);

    if (!accepting_.load(std::memory_order_acquire)) {
      return;
    }

    if (LevelValue(level) < LevelValue(min_level_.load(std::memory_order_acquire))) {
      filtered_by_level_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    StartWorkerIfNeeded();

    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    entry.tid = CurrentThreadId();
    entry.file_name = Basename(location.file_name());
    entry.line = location.line();
    entry.function_name = location.function_name();
    entry.message.assign(message.data(), message.size());

    for (int retry = 0; retry < kPushRetryLimit; ++retry) {
      if (queue_.TryPush(std::move(entry))) {
        enqueued_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      if (retry + 1 < kPushRetryLimit) {
        std::this_thread::yield();
      }
    }

    dropped_.fetch_add(1, std::memory_order_relaxed);
    dropped_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void Shutdown() noexcept {
    bool expected = true;
    if (!accepting_.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
      return;
    }

    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }

    CloseFile();
  }

  static AsyncLoggerEngine& Instance() {
    static AsyncLoggerEngine instance;
    return instance;
  }

 private:
  void StartWorkerIfNeeded() {
    bool expected = false;
    if (!worker_started_.compare_exchange_strong(expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
      return;
    }

    worker_ = std::jthread(
        [this](std::stop_token stop_token) { WorkerMain(stop_token); });
  }

  void RegisterAtExit() {
    bool expected = false;
    if (g_atexit_registered.compare_exchange_strong(expected, true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_relaxed)) {
      std::atexit([] {
        AsyncLoggerEngine::Instance().Shutdown();
      });
    }
  }

  [[nodiscard]] std::chrono::milliseconds FlushInterval() const noexcept {
    return std::chrono::milliseconds(
        flush_interval_ms_.load(std::memory_order_acquire));
  }

  [[nodiscard]] std::string PendingPath() {
    std::lock_guard<std::mutex> lock(path_mu_);
    return pending_file_path_;
  }

  void ReopenFileIfRequested() {
    if (!reopen_requested_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    OpenFile(PendingPath());
  }

  void OpenFile(std::string path) {
    if (path.empty()) {
      path = std::string(kDefaultLogFile);
    }

    CloseFile();

    file_ = std::fopen(path.c_str(), "ab");
    if (file_ == nullptr) {
      WriteFallback(std::string("[LOGGER][ERROR] open file failed: ") + path + "\n");
      return;
    }

    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
  }

  void CloseFile() noexcept {
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  void WorkerMain(std::stop_token stop_token) {
    OpenFile(PendingPath());

    std::array<LogEntry, kBatchSize> batch;
    std::size_t idle_spins = 0;
    auto last_flush = std::chrono::steady_clock::now();

    while (!stop_token.stop_requested()) {
      ReopenFileIfRequested();

      const std::size_t n = queue_.PopBatch(std::span<LogEntry>(batch));
      if (n > 0) {
        idle_spins = 0;
        consumed_.fetch_add(static_cast<std::uint64_t>(n),
                            std::memory_order_relaxed);
        WriteBatch(std::span<const LogEntry>(batch.data(), n));
      } else {
        if (idle_spins < kSpinBeforeSleep) {
          ++idle_spins;
          std::this_thread::yield();
        } else {
          std::this_thread::sleep_for(kIdleSleep);
        }
      }

      EmitDroppedSummaryIfNeeded(false);

      const auto now = std::chrono::steady_clock::now();
      if (flush_requested_.exchange(false, std::memory_order_acq_rel) ||
          now - last_flush >= FlushInterval()) {
        FlushFile();
        last_flush = now;
      }
    }

    // stop requested: drain remaining entries for graceful shutdown.
    DrainRemaining(batch);
    EmitDroppedSummaryIfNeeded(true);
    FlushFile();
  }

  void DrainRemaining(std::array<LogEntry, kBatchSize>& buffer) {
    while (true) {
      const std::size_t n = queue_.PopBatch(std::span<LogEntry>(buffer));
      if (n == 0) {
        break;
      }
      WriteBatch(std::span<const LogEntry>(buffer.data(), n));
    }
  }

  void EmitDroppedSummaryIfNeeded(bool force_emit) {
    pending_drop_report_ +=
        dropped_count_.exchange(0, std::memory_order_acq_rel);

    if (pending_drop_report_ == 0) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force_emit && now - last_drop_report_time_ < kDroppedReportInterval) {
      return;
    }

    LogEntry summary;
    summary.timestamp = std::chrono::system_clock::now();
    summary.level = LogLevel::kWarn;
    summary.tid = CurrentThreadId();
    summary.file_name = "async_logger";
    summary.function_name = "WorkerMain";
    summary.message = "dropped " + std::to_string(pending_drop_report_) +
                      " log entries because queue is full";
    pending_drop_report_ = 0;
    last_drop_report_time_ = now;
    WriteBatch(std::span<const LogEntry>(&summary, 1));
  }

  void WriteBatch(std::span<const LogEntry> batch) {
    if (batch.empty()) {
      return;
    }

    write_buffer_.clear();
    write_buffer_.reserve(batch.size() * 200);

    for (const auto& entry : batch) {
      AppendFormattedEntry(entry);
    }

    if (file_ != nullptr) {
      const std::size_t written =
          std::fwrite(write_buffer_.data(), 1, write_buffer_.size(), file_);
      if (written == write_buffer_.size()) {
        return;
      }
      WriteFallback(std::string_view(write_buffer_).substr(written));
      return;
    }

    WriteFallback(write_buffer_);
  }

  void AppendFormattedEntry(const LogEntry& entry) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.timestamp.time_since_epoch()) %
                    1000;
    const std::time_t sec = std::chrono::system_clock::to_time_t(entry.timestamp);

    if (sec != cached_second_) {
      std::tm local_tm{};
#if defined(_WIN32)
      localtime_s(&local_tm, &sec);
#else
      localtime_r(&sec, &local_tm);
#endif
      if (std::strftime(cached_second_text_.data(), cached_second_text_.size(),
                        "%Y-%m-%d %H:%M:%S", &local_tm) == 0) {
        std::snprintf(cached_second_text_.data(), cached_second_text_.size(),
                      "1970-01-01 00:00:00");
      }
      cached_second_ = sec;
    }

    char prefix[768] = {0};
    std::snprintf(prefix, sizeof(prefix),
                  "[%s.%03d][%s][tid=%llu] %s:%u %s | ",
                  cached_second_text_.data(), static_cast<int>(ms.count()),
                  LogLevelName(entry.level),
                  static_cast<unsigned long long>(entry.tid), entry.file_name,
                  static_cast<unsigned int>(entry.line), entry.function_name);

    write_buffer_.append(prefix);
    write_buffer_.append(entry.message);
    write_buffer_.push_back('\n');
  }

  void WriteFallback(std::string_view data) noexcept {
    std::fwrite(data.data(), 1, data.size(), stderr);
  }

  void FlushFile() noexcept {
    if (file_ != nullptr) {
      std::fflush(file_);
    }
  }

  MpscRingQueue<LogEntry, kQueueCapacity> queue_;

  std::atomic<LogLevel> min_level_{LogLevel::kInfo};
  std::atomic<std::int64_t> flush_interval_ms_{
      kDefaultFlushInterval.count()};
  std::atomic<std::uint64_t> submit_calls_{0};
  std::atomic<std::uint64_t> filtered_by_level_{0};
  std::atomic<std::uint64_t> enqueued_{0};
  std::atomic<std::uint64_t> consumed_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> dropped_count_{0};
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> reopen_requested_{false};
  std::atomic<bool> accepting_{true};
  std::atomic<bool> worker_started_{false};

  std::mutex path_mu_;
  std::string pending_file_path_{std::string(kDefaultLogFile)};

  std::FILE* file_{nullptr};
  std::jthread worker_;
  std::string write_buffer_;
  std::time_t cached_second_{static_cast<std::time_t>(-1)};
  std::array<char, 32> cached_second_text_{};
  std::uint64_t pending_drop_report_{0};
  std::chrono::steady_clock::time_point last_drop_report_time_{};
};

}  // namespace

void InitAsyncLogger(const LoggerOptions& options) {
  AsyncLoggerEngine::Instance().Init(options);
}

void SetAsyncLoggerFile(std::string_view path) {
  AsyncLoggerEngine::Instance().SetLogFile(path);
}

void SetAsyncLoggerLevel(LogLevel level) noexcept {
  AsyncLoggerEngine::Instance().SetLogLevel(level);
}

LogLevel GetAsyncLoggerLevel() noexcept {
  return AsyncLoggerEngine::Instance().GetLogLevel();
}

LoggerRuntimeStats GetAsyncLoggerRuntimeStats() noexcept {
  return AsyncLoggerEngine::Instance().GetRuntimeStats();
}

void FlushAsyncLogger() noexcept { AsyncLoggerEngine::Instance().Flush(); }

void ShutdownAsyncLogger() noexcept { AsyncLoggerEngine::Instance().Shutdown(); }

void SubmitAsyncLog(LogLevel level, std::string_view message,
                    const std::source_location& location) noexcept {
  AsyncLoggerEngine::Instance().Submit(level, message, location);
}

}  // namespace rpc::common::detail
