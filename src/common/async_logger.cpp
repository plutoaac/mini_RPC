#include "common/async_logger.h"

#include "common/mpsc_ring_queue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rpc::common::detail {
namespace {

constexpr std::size_t kRingCapacity = 1u << 18;
constexpr std::size_t kDefaultQueueCapacity = 1u << 18;
constexpr std::size_t kDefaultBatchSize = 1024;
constexpr std::chrono::milliseconds kDefaultFlushInterval{500};
constexpr std::chrono::microseconds kDefaultIdleSleep{50};
constexpr std::chrono::milliseconds kDropSummaryInterval{500};
constexpr std::size_t kDefaultInlineMessageCapacity = 112;
constexpr std::string_view kDefaultLogFilePath = "./rpc.log";

// Global one-time atexit registration guard.
constinit std::atomic<bool> g_atexit_registered{false};

struct ThreadIdCache final {
  std::array<char, 24> text{};
  std::uint8_t size{1};
};

[[nodiscard]] const ThreadIdCache& CurrentThreadIdCache() noexcept {
  static thread_local const ThreadIdCache cache = [] {
    ThreadIdCache result;
    const std::uint64_t tid_hash = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto [ptr, ec] = std::to_chars(
        result.text.data(), result.text.data() + result.text.size(), tid_hash);
    if (ec == std::errc{}) {
      result.size = static_cast<std::uint8_t>(ptr - result.text.data());
      return result;
    }
    result.text[0] = '0';
    result.size = 1;
    return result;
  }();

  return cache;
}

[[nodiscard]] std::string TrimPath(std::string_view raw) {
  auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };

  auto begin = raw.begin();
  while (begin != raw.end() && is_space(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  if (begin == raw.end()) {
    return {};
  }

  auto end = raw.end();
  while (end != begin) {
    const char ch = static_cast<char>(*(end - 1));
    if (!is_space(static_cast<unsigned char>(ch))) {
      break;
    }
    --end;
  }

  return std::string(begin, end);
}

[[nodiscard]] constexpr std::uint8_t LevelValue(LogLevel level) noexcept {
  return static_cast<std::uint8_t>(level);
}

void AppendUnsigned(std::string& out, std::uint64_t value) {
  char digits[24] = {0};
  const auto [ptr, ec] =
      std::to_chars(digits, digits + sizeof(digits), value);
  if (ec == std::errc{}) {
    out.append(digits, ptr);
    return;
  }
  out.push_back('0');
}

void AppendMilliseconds3(std::string& out, int ms) {
  const int safe_ms = std::max(0, std::min(999, ms));
  out.push_back(static_cast<char>('0' + (safe_ms / 100)));
  out.push_back(static_cast<char>('0' + ((safe_ms / 10) % 10)));
  out.push_back(static_cast<char>('0' + (safe_ms % 10)));
}

class SmallLogMessage final {
 public:
  static constexpr std::size_t kInlineCapacity = 112;

  SmallLogMessage() = default;
  ~SmallLogMessage() = default;

  SmallLogMessage(SmallLogMessage&&) noexcept = default;
  SmallLogMessage& operator=(SmallLogMessage&&) noexcept = default;

  SmallLogMessage(const SmallLogMessage&) = delete;
  SmallLogMessage& operator=(const SmallLogMessage&) = delete;

  void Assign(std::string_view message, std::size_t inline_limit) {
    const std::size_t threshold =
        std::max<std::size_t>(1, std::min(inline_limit, kInlineCapacity));

    if (message.size() <= threshold) {
      using_heap_ = false;
      inline_size_ = static_cast<std::uint16_t>(message.size());
      if (!message.empty()) {
        std::memcpy(inline_buffer_.data(), message.data(), message.size());
      }
      heap_buffer_.clear();
      return;
    }

    using_heap_ = true;
    inline_size_ = 0;
    heap_buffer_.assign(message.data(), message.size());
  }

  [[nodiscard]] std::string_view View() const noexcept {
    if (using_heap_) {
      return heap_buffer_;
    }
    return std::string_view(inline_buffer_.data(), inline_size_);
  }

 private:
  bool using_heap_{false};
  std::uint16_t inline_size_{0};
  std::array<char, kInlineCapacity> inline_buffer_{};
  std::string heap_buffer_;
};

struct LogEntry final {
  std::chrono::system_clock::time_point timestamp{
      std::chrono::system_clock::now()};
  LogLevel level{LogLevel::kInfo};
  std::array<char, 24> tid_text{};
  std::uint8_t tid_size{1};
  const char* file_name{"unknown"};
  std::uint_least32_t line{0};
  const char* function_name{"unknown"};
  SmallLogMessage message;
};

static_assert(std::is_nothrow_move_constructible_v<LogEntry>);
static_assert(std::is_nothrow_move_assignable_v<LogEntry>);

class AsyncLoggerEngine final {
 public:
  AsyncLoggerEngine() {
    StartWorkerIfNeeded();
    RegisterAtExit();
  }

  ~AsyncLoggerEngine() { Shutdown(); }

  void Init(const LoggerOptions& options) {
    SetLogLevel(options.min_level);

    queue_capacity_soft_.store(
        std::max<std::size_t>(1, std::min(options.queue_capacity, kRingCapacity)),
        std::memory_order_release);
    max_batch_size_.store(
        std::max<std::size_t>(1, options.max_batch_size), std::memory_order_release);
    flush_interval_ms_.store(
        std::max<std::int64_t>(1, options.flush_interval.count()),
        std::memory_order_release);
    spin_before_sleep_.store(
        std::max<std::uint32_t>(1, options.spin_before_sleep),
        std::memory_order_release);
    idle_sleep_ns_.store(
        std::max<std::int64_t>(0, options.idle_sleep.count()),
        std::memory_order_release);
    inline_message_capacity_.store(
        std::max<std::size_t>(1,
                              std::min(options.inline_message_capacity,
                                       SmallLogMessage::kInlineCapacity)),
        std::memory_order_release);
    drop_policy_.store(options.drop_policy, std::memory_order_release);

    const std::string resolved_path = ResolveLogPath(options);
    {
      std::lock_guard<std::mutex> lock(path_mu_);
      pending_log_path_ = resolved_path;
    }
    reopen_requested_.store(true, std::memory_order_release);

    StartWorkerIfNeeded();
  }

  void SetLogFile(std::string_view path) {
    const std::string normalized = TrimPath(path);
    if (normalized.empty()) {
      return;
    }

    {
      std::lock_guard<std::mutex> lock(path_mu_);
      pending_log_path_ = normalized;
    }
    reopen_requested_.store(true, std::memory_order_release);
  }

  void SetLogLevel(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_release);
  }

  [[nodiscard]] LogLevel GetLogLevel() noexcept {
    return min_level_.load(std::memory_order_acquire);
  }

  [[nodiscard]] LoggerRuntimeStats GetRuntimeStats() noexcept {
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

    if (!accepting_logs_.load(std::memory_order_acquire)) {
      return;
    }

    // Fast path level filtering: filtered logs never touch the queue.
    if (LevelValue(level) < LevelValue(min_level_.load(std::memory_order_acquire))) {
      filtered_by_level_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const std::size_t soft_cap = queue_capacity_soft_.load(std::memory_order_relaxed);
    if (soft_cap < kRingCapacity &&
      inflight_count_.load(std::memory_order_relaxed) >= soft_cap) {
      // DropOldest is intentionally mapped to DropNewest in this phase because
      // lock-free MPSC fast path should not mutate old slots.
      dropped_.fetch_add(1, std::memory_order_relaxed);
      dropped_for_summary_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    LogEntry entry;
    try {
      entry.timestamp = std::chrono::system_clock::now();
      entry.level = level;
      const auto& tid = CurrentThreadIdCache();
      entry.tid_size = tid.size;
      std::memcpy(entry.tid_text.data(), tid.text.data(), tid.size);
      entry.file_name = Basename(location.file_name());
      entry.line = location.line();
      entry.function_name = location.function_name();
      // Phase 2 core change: producer captures structured fields only.
      entry.message.Assign(
          message, inline_message_capacity_.load(std::memory_order_relaxed));
    } catch (...) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      dropped_for_summary_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (queue_.TryPush(std::move(entry))) {
      inflight_count_.fetch_add(1, std::memory_order_relaxed);
      enqueued_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    dropped_.fetch_add(1, std::memory_order_relaxed);
    dropped_for_summary_.fetch_add(1, std::memory_order_relaxed);
  }

  void Shutdown() noexcept {
    if (!accepting_logs_.exchange(false, std::memory_order_acq_rel)) {
      return;
    }

    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }

    CloseFile();
  }

  static AsyncLoggerEngine& Instance() {
    static AsyncLoggerEngine engine;
    return engine;
  }

 private:
  void StartWorkerIfNeeded() {
    bool expected = false;
    if (!worker_started_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return;
    }

    // jthread + stop_token gives clear ownership and shutdown semantics.
    worker_ = std::jthread(
        [this](std::stop_token token) { WorkerLoop(token); });
  }

  void RegisterAtExit() {
    bool expected = false;
    if (g_atexit_registered.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      std::atexit([] { AsyncLoggerEngine::Instance().Shutdown(); });
    }
  }

  [[nodiscard]] static std::string ResolveLogPath(const LoggerOptions& options) {
    const std::string compat = TrimPath(options.file_path);
    if (!compat.empty()) {
      return compat;
    }

    if (!options.log_file_path.empty()) {
      return options.log_file_path.string();
    }

    return std::string(kDefaultLogFilePath);
  }

  [[nodiscard]] std::string CurrentLogPath() {
    std::lock_guard<std::mutex> lock(path_mu_);
    return pending_log_path_;
  }

  void WorkerLoop(std::stop_token stop_token) {
    OpenFile(CurrentLogPath());

    std::vector<LogEntry> batch;
    batch.resize(max_batch_size_.load(std::memory_order_acquire));

    auto last_flush = std::chrono::steady_clock::now();
    auto last_drop_report = std::chrono::steady_clock::now();

    for (;;) {
      if (reopen_requested_.exchange(false, std::memory_order_acq_rel)) {
        OpenFile(CurrentLogPath());
      }

      const std::size_t batch_limit =
          max_batch_size_.load(std::memory_order_acquire);
      if (batch.size() != batch_limit) {
        batch.resize(batch_limit);
      }

      const std::size_t drained = queue_.PopBatch(
          std::span<LogEntry>(batch.data(), batch.size()));

      if (drained > 0) {
        inflight_count_.fetch_sub(drained, std::memory_order_relaxed);
        consumed_.fetch_add(drained, std::memory_order_relaxed);

        FormatBatch(std::span<const LogEntry>(batch.data(), drained),
                    formatted_batch_buffer_);
        WriteBuffer(formatted_batch_buffer_);

        const auto now = std::chrono::steady_clock::now();
        if (flush_requested_.exchange(false, std::memory_order_acq_rel) ||
            now - last_flush >= FlushInterval()) {
          FlushFile();
          last_flush = now;
        }

        EmitDropSummaryIfNeeded(false, last_drop_report);
        continue;
      }

      EmitDropSummaryIfNeeded(false, last_drop_report);

      const auto now = std::chrono::steady_clock::now();
      if (flush_requested_.exchange(false, std::memory_order_acq_rel) ||
          now - last_flush >= FlushInterval()) {
        FlushFile();
        last_flush = now;
      }

      if (stop_token.stop_requested()) {
        break;
      }

      const std::uint32_t spins = spin_before_sleep_.load(std::memory_order_relaxed);
      bool has_work = false;
      for (std::uint32_t i = 0; i < spins; ++i) {
        if (inflight_count_.load(std::memory_order_relaxed) > 0 ||
            reopen_requested_.load(std::memory_order_relaxed) ||
            flush_requested_.load(std::memory_order_relaxed)) {
          has_work = true;
          break;
        }
        std::this_thread::yield();
      }

      if (!has_work) {
        const auto sleep_ns = idle_sleep_ns_.load(std::memory_order_relaxed);
        if (sleep_ns > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(sleep_ns));
        }
      }
    }

    while (true) {
      const std::size_t drained = queue_.PopBatch(
          std::span<LogEntry>(batch.data(), batch.size()));
      if (drained == 0) {
        break;
      }
      inflight_count_.fetch_sub(drained, std::memory_order_relaxed);
      consumed_.fetch_add(drained, std::memory_order_relaxed);
      FormatBatch(std::span<const LogEntry>(batch.data(), drained),
                  formatted_batch_buffer_);
      WriteBuffer(formatted_batch_buffer_);
    }

    EmitDropSummaryIfNeeded(true, last_drop_report);
    FlushFile();
  }

  void FormatBatch(std::span<const LogEntry> batch, std::string& out) {
    out.clear();
    out.reserve(batch.size() * 192);

    for (const auto& entry : batch) {
      AppendFormattedEntry(entry, out);
    }
  }

  void AppendFormattedEntry(const LogEntry& entry, std::string& out) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.timestamp.time_since_epoch()) %
                    1000;
    const std::time_t sec = std::chrono::system_clock::to_time_t(entry.timestamp);

    // Second-level cache avoids full strftime for every log line.
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

    out.push_back('[');
    out.append(cached_second_text_.data());
    out.push_back('.');
    AppendMilliseconds3(out, static_cast<int>(ms.count()));
    out.append("][");
    out.append(LogLevelName(entry.level));
    out.append("][tid=");
    out.append(entry.tid_text.data(), entry.tid_size);
    out.append("] ");
    out.append(entry.file_name);
    out.push_back(':');
    AppendUnsigned(out, static_cast<std::uint64_t>(entry.line));
    out.push_back(' ');
    out.append(entry.function_name);
    out.append(" | ");
    out.append(entry.message.View());
    out.push_back('\n');
  }

  void WriteBuffer(std::string_view bytes) noexcept {
    if (bytes.empty()) {
      return;
    }

    if (file_ != nullptr) {
      const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file_);
      if (written == bytes.size()) {
        return;
      }
      WriteFallback(bytes.substr(written));
      return;
    }

    WriteFallback(bytes);
  }

  void EmitDropSummaryIfNeeded(
      bool force_emit,
      std::chrono::steady_clock::time_point& last_drop_report) {
    pending_drop_summary_ +=
        dropped_for_summary_.exchange(0, std::memory_order_acq_rel);

    if (pending_drop_summary_ == 0) {
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force_emit && now - last_drop_report < kDropSummaryInterval) {
      return;
    }
    last_drop_report = now;

    LogEntry summary;
    summary.timestamp = std::chrono::system_clock::now();
    summary.level = LogLevel::kWarn;
    const auto& tid = CurrentThreadIdCache();
    summary.tid_size = tid.size;
    std::memcpy(summary.tid_text.data(), tid.text.data(), tid.size);
    summary.file_name = "async_logger";
    summary.function_name = "EmitDropSummaryIfNeeded";

    const std::string text =
        "dropped " + std::to_string(pending_drop_summary_) +
        " log entries because queue is full";
    summary.message.Assign(text, SmallLogMessage::kInlineCapacity);

    pending_drop_summary_ = 0;

    std::array<LogEntry, 1> one{std::move(summary)};
    FormatBatch(std::span<const LogEntry>(one.data(), one.size()),
                formatted_batch_buffer_);
    WriteBuffer(formatted_batch_buffer_);
  }

  [[nodiscard]] std::chrono::milliseconds FlushInterval() const noexcept {
    return std::chrono::milliseconds(
        flush_interval_ms_.load(std::memory_order_acquire));
  }

  void OpenFile(const std::string& path) {
    std::string resolved = path;
    if (resolved.empty()) {
      resolved = std::string(kDefaultLogFilePath);
    }

    CloseFile();

    file_ = std::fopen(resolved.c_str(), "ab");
    if (file_ == nullptr) {
      WriteFallback(std::string("[LOGGER][ERROR] open file failed: ") + resolved + "\n");
      return;
    }

    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
  }

  void FlushFile() noexcept {
    if (file_ != nullptr) {
      std::fflush(file_);
    }
  }

  void CloseFile() noexcept {
    if (file_ != nullptr) {
      std::fflush(file_);
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  void WriteFallback(std::string_view bytes) noexcept {
    if (bytes.empty()) {
      return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), stderr);
  }

  MpscRingQueue<LogEntry, kRingCapacity> queue_;

  std::atomic<std::size_t> inflight_count_{0};
  std::atomic<std::size_t> queue_capacity_soft_{kDefaultQueueCapacity};
  std::atomic<std::size_t> max_batch_size_{kDefaultBatchSize};
  std::atomic<std::int64_t> flush_interval_ms_{kDefaultFlushInterval.count()};
  std::atomic<std::uint32_t> spin_before_sleep_{256};
  std::atomic<std::int64_t> idle_sleep_ns_{kDefaultIdleSleep.count()};
  std::atomic<std::size_t> inline_message_capacity_{kDefaultInlineMessageCapacity};

  std::atomic<DropPolicy> drop_policy_{DropPolicy::DropNewest};
  std::atomic<LogLevel> min_level_{LogLevel::kInfo};

  std::atomic<bool> accepting_logs_{true};
  std::atomic<bool> worker_started_{false};
  std::atomic<bool> flush_requested_{false};
  std::atomic<bool> reopen_requested_{false};

  std::atomic<std::uint64_t> submit_calls_{0};
  std::atomic<std::uint64_t> filtered_by_level_{0};
  std::atomic<std::uint64_t> enqueued_{0};
  std::atomic<std::uint64_t> consumed_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> dropped_for_summary_{0};

  std::uint64_t pending_drop_summary_{0};

  std::mutex path_mu_;
  std::string pending_log_path_{std::string(kDefaultLogFilePath)};

  std::FILE* file_{nullptr};
  std::jthread worker_;

  std::string formatted_batch_buffer_;

  std::time_t cached_second_{static_cast<std::time_t>(-1)};
  std::array<char, 32> cached_second_text_{};
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
