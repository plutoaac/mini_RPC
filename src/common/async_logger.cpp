// ============================================================================
// async_logger.cpp —— 高性能异步日志引擎实现
// ============================================================================
// 核心设计：
//   1. 单例模式（Meyer's Singleton）保证全局唯一的 AsyncLoggerEngine 实例。
//   2. 生产者-消费者模型：主线程通过无锁 MPSC 环形队列（MpscRingQueue）
//      提交 LogEntry；后台 worker 线程批量消费、格式化并写入文件。
//   3. 小对象优化（SSO）：SmallLogMessage 对短消息使用栈上内联缓冲区，
//      避免堆分配；长消息自动降级到 std::string。
//   4. 秒级缓存：AppendFormattedEntry 缓存 strftime 结果，减少时间格式化开销。
//   5. 可配置丢弃策略：当队列超过软上限时，新日志被丢弃（当前版本 DropOldest
//      与 DropNewest 行为一致，均为丢弃 newest）。
//   6. 优雅关闭：通过 std::jthread + stop_token 实现，atexit 注册确保进程退出
//      时自动刷盘、释放资源。
// ============================================================================

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

// ----------------------------------------------------------------------------
// 全局常量
// ----------------------------------------------------------------------------

// 底层 MPSC 环形队列的固定容量（2^18 = 262144）。这是硬上限，Init() 传入的
// queue_capacity 不能超过此值。
constexpr std::size_t kRingCapacity = 1u << 18;
// 默认队列软上限（2^16 = 65536）。可通过 Init() 覆盖，用于触发丢弃逻辑。
constexpr std::size_t kDefaultQueueCapacity = 1u << 16;
// 默认每次 worker 循环最多批量消费 512 条日志。
constexpr std::size_t kDefaultBatchSize = 512;
// 默认自动 flush 间隔：500 ms。
constexpr std::chrono::milliseconds kDefaultFlushInterval{500};
// 默认 worker 空闲时休眠 150 微秒，避免 CPU 空转。
constexpr std::chrono::microseconds kDefaultIdleSleep{150};
// 丢弃摘要日志的最小输出间隔：500 ms。
constexpr std::chrono::milliseconds kDropSummaryInterval{500};
// 默认内联消息长度阈值：112 字节。小于等于此长度的日志直接存于 SmallLogMessage
// 的栈缓冲区，无需堆分配。
constexpr std::size_t kDefaultInlineMessageCapacity = 112;
// 默认日志文件路径。
constexpr std::string_view kDefaultLogFilePath = "./rpc.log";

// 全局标志：确保 atexit 只注册一次，防止重复注册导致未定义行为。
constinit std::atomic<bool> g_atexit_registered{false};

// ----------------------------------------------------------------------------
// ThreadIdCache —— 线程 ID 缓存
// ----------------------------------------------------------------------------
// 每个线程首次调用 CurrentThreadIdCache() 时，将 std::thread::id 的 hash 值
// 转成十进制字符串并缓存到 thread_local 变量中，避免每次日志都重新格式化。
// ----------------------------------------------------------------------------
struct ThreadIdCache final {
  std::array<char, 24> text{};  // 十进制字符串缓冲区
  std::uint8_t size{1};         // 实际长度
};

[[nodiscard]] const ThreadIdCache& CurrentThreadIdCache() noexcept {
  static thread_local const ThreadIdCache cache = [] {
    ThreadIdCache result;
    // 使用 std::hash<std::thread::id> 生成 64 位 hash 值作为线程标识。
    const std::uint64_t tid_hash = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto [ptr, ec] = std::to_chars(
        result.text.data(), result.text.data() + result.text.size(), tid_hash);
    if (ec == std::errc{}) {
      result.size = static_cast<std::uint8_t>(ptr - result.text.data());
      return result;
    }
    // 转换失败时的安全回退。
    result.text[0] = '0';
    result.size = 1;
    return result;
  }();

  return cache;
}

// ----------------------------------------------------------------------------
// TrimPath —— 去除字符串两端空白字符
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// LevelValue —— 将 LogLevel 枚举转成底层整数值，用于快速比较
// ----------------------------------------------------------------------------
[[nodiscard]] constexpr std::uint8_t LevelValue(LogLevel level) noexcept {
  return static_cast<std::uint8_t>(level);
}

// ----------------------------------------------------------------------------
// AppendUnsigned —— 将无符号整数追加到 std::string（无动态格式化开销）
// ----------------------------------------------------------------------------
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

// ----------------------------------------------------------------------------
// AppendMilliseconds3 —— 将毫秒值格式化为固定 3 位数字（001 ~ 999）追加到字符串
// ----------------------------------------------------------------------------
void AppendMilliseconds3(std::string& out, int ms) {
  const int safe_ms = std::max(0, std::min(999, ms));
  out.push_back(static_cast<char>('0' + (safe_ms / 100)));
  out.push_back(static_cast<char>('0' + ((safe_ms / 10) % 10)));
  out.push_back(static_cast<char>('0' + (safe_ms % 10)));
}

// ----------------------------------------------------------------------------
// SmallLogMessage —— 小对象优化的日志消息容器
// ----------------------------------------------------------------------------
// 消息长度 <= kInlineCapacity（默认 112）时，直接存储在栈数组 inline_buffer_ 中，
// 避免 new/delete 或 malloc/free；超长消息自动降级到堆上的 std::string。
// 这是实现零分配（zero-allocation）快速路径的关键。
// ----------------------------------------------------------------------------
class SmallLogMessage final {
 public:
  static constexpr std::size_t kInlineCapacity = 112;

  SmallLogMessage() = default;
  ~SmallLogMessage() = default;

  SmallLogMessage(SmallLogMessage&&) noexcept = default;
  SmallLogMessage& operator=(SmallLogMessage&&) noexcept = default;

  SmallLogMessage(const SmallLogMessage&) = delete;
  SmallLogMessage& operator=(const SmallLogMessage&) = delete;

  // 根据 inline_limit 决定将 message 存于栈缓冲区还是堆缓冲区。
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

  // 返回消息的只读视图，无需关心内部存储位置。
  [[nodiscard]] std::string_view View() const noexcept {
    if (using_heap_) {
      return heap_buffer_;
    }
    return std::string_view(inline_buffer_.data(), inline_size_);
  }

 private:
  bool using_heap_{false};                // 当前是否使用堆存储
  std::uint16_t inline_size_{0};          // 栈缓冲区的实际长度
  std::array<char, kInlineCapacity> inline_buffer_{};
  std::string heap_buffer_;               // 超长消息的堆后备
};

// ----------------------------------------------------------------------------
// LogEntry —— 单条日志的结构化记录
// ----------------------------------------------------------------------------
// 生产者在 Submit() 中填充此结构并推入 MPSC 队列；消费者取出后格式化输出。
// 字段尽可能紧凑，且均为 noexcept-move，确保队列操作高效。
// ----------------------------------------------------------------------------
struct LogEntry final {
  std::chrono::system_clock::time_point timestamp{
      std::chrono::system_clock::now()};
  LogLevel level{LogLevel::kInfo};
  std::array<char, 24> tid_text{};        // 线程 ID 字符串
  std::uint8_t tid_size{1};               // 线程 ID 长度
  const char* file_name{"unknown"};       // 源文件名（建议为字面量/静态字符串）
  std::uint_least32_t line{0};            // 行号
  const char* function_name{"unknown"};   // 函数名
  SmallLogMessage message;                // 日志正文（含 SSO）
};

static_assert(std::is_nothrow_move_constructible_v<LogEntry>);
static_assert(std::is_nothrow_move_assignable_v<LogEntry>);

// ----------------------------------------------------------------------------
// AsyncLoggerEngine —— 异步日志核心引擎（单例）
// ----------------------------------------------------------------------------
// 职责划分：
//   - 生产者侧（Submit）：快速路径为无锁队列 TryPush + 原子计数器更新。
//   - 消费者侧（WorkerLoop）：后台线程批量 PopBatch → 格式化 → 写入文件。
//   - 生命周期：Init() 可重复调用以更新参数，但 Shutdown() 为终态，
//     一旦关闭便不再接受新日志（符合 Phase 2.5 设计）。
// ----------------------------------------------------------------------------
class AsyncLoggerEngine final {
 public:
  AsyncLoggerEngine() {
    StartWorkerIfNeeded();
    RegisterAtExit();
  }

  ~AsyncLoggerEngine() { Shutdown(); }

  // --------------------------------------------------------------------------
  // Init —— 使用 LoggerOptions 初始化或重新配置日志引擎
  // --------------------------------------------------------------------------
  // 注意：当前单例生命周期为一次性；Shutdown() 之后不会重新启动 worker。
  // 各参数均会经过 clamp（钳制）到合法范围，再写入原子变量供 worker 读取。
  // --------------------------------------------------------------------------
  void Init(const LoggerOptions& options) {
    if (!accepting_logs_.load(std::memory_order_acquire)) {
      return;
    }

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
    idle_sleep_us_.store(
        std::max<std::int64_t>(0, options.idle_sleep.count()),
        std::memory_order_release);
    inline_message_threshold_.store(
      std::max<std::size_t>(1,
                  std::min(options.inline_message_threshold,
                       SmallLogMessage::kInlineCapacity)),
        std::memory_order_release);
    drop_policy_.store(options.drop_policy, std::memory_order_release);

    const std::string resolved_path = ResolveLogPath(options);
    {
      std::lock_guard<std::mutex> lock(path_mu_);
      pending_log_path_ = resolved_path;
    }
    // 通知 worker 线程下一次循环时重新打开文件。
    reopen_requested_.store(true, std::memory_order_release);

    StartWorkerIfNeeded();
  }

  // --------------------------------------------------------------------------
  // SetLogFile —— 动态切换日志文件路径
  // --------------------------------------------------------------------------
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

  // --------------------------------------------------------------------------
  // SetLogLevel / GetLogLevel —— 动态调整最低日志级别
  // --------------------------------------------------------------------------
  void SetLogLevel(LogLevel level) noexcept {
    min_level_.store(level, std::memory_order_release);
  }

  [[nodiscard]] LogLevel GetLogLevel() noexcept {
    return min_level_.load(std::memory_order_acquire);
  }

  // --------------------------------------------------------------------------
  // GetRuntimeStats —— 获取运行期统计信息（用于监控与调优）
  // --------------------------------------------------------------------------
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

  // --------------------------------------------------------------------------
  // Flush —— 请求立刻刷盘（由 worker 线程在合适时机执行）
  // --------------------------------------------------------------------------
  void Flush() noexcept { flush_requested_.store(true, std::memory_order_release); }

  // --------------------------------------------------------------------------
  // Submit —— 生产者入口，供宏或包装函数调用
  // --------------------------------------------------------------------------
  // 执行路径：
  //   1. 原子递增 submit_calls_。
  //   2. 若已 Shutdown（accepting_logs_ == false），直接返回。
  //   3. 快速级别过滤：低于 min_level_ 的日志不触碰队列。
  //   4. 软上限检查：若 inflight_count_ >= queue_capacity_soft_，则丢弃。
  //   5. 填充 LogEntry（捕获时间、线程ID、源码位置、消息内容）。
  //   6. 尝试无锁入队（TryPush）；失败则计为丢弃。
  // --------------------------------------------------------------------------
  void Submit(LogLevel level, std::string_view message,
              const std::source_location& location) noexcept {
    submit_calls_.fetch_add(1, std::memory_order_relaxed);

    if (!accepting_logs_.load(std::memory_order_acquire)) {
      return;
    }

    // 快速路径级别过滤：被过滤的日志完全不触碰队列，零开销。
    if (LevelValue(level) < LevelValue(min_level_.load(std::memory_order_acquire))) {
      filtered_by_level_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const std::size_t soft_cap = queue_capacity_soft_.load(std::memory_order_relaxed);
    if (soft_cap < kRingCapacity &&
      inflight_count_.load(std::memory_order_relaxed) >= soft_cap) {
      // Phase 2.5 语义说明：DropOldest 预留用于后续扩展；
      // 当前无锁 MPSC 快速路径下，两种策略行为均为 DropNewest。
      dropped_.fetch_add(1, std::memory_order_relaxed);
      dropped_for_summary_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    LogEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.level = level;
    const auto& tid = CurrentThreadIdCache();
    entry.tid_size = tid.size;
    std::memcpy(entry.tid_text.data(), tid.text.data(), tid.size);
    entry.file_name = Basename(location.file_name());
    entry.line = location.line();
    entry.function_name = location.function_name();
    // Phase 2 核心变更：生产者仅捕获结构化字段，格式化延后到消费者线程。
    entry.message.Assign(
        message, inline_message_threshold_.load(std::memory_order_relaxed));

    if (queue_.TryPush(std::move(entry))) {
      inflight_count_.fetch_add(1, std::memory_order_relaxed);
      enqueued_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    // 队列已满（硬上限），无锁入队失败。
    dropped_.fetch_add(1, std::memory_order_relaxed);
    dropped_for_summary_.fetch_add(1, std::memory_order_relaxed);
  }

  // --------------------------------------------------------------------------
  // Shutdown —— 优雅关闭：停止接收新日志、唤醒并等待 worker 结束、关闭文件
  // --------------------------------------------------------------------------
  void Shutdown() noexcept {
    if (!accepting_logs_.exchange(false, std::memory_order_acq_rel)) {
      return;  // 已经关闭，幂等
    }

    if (worker_.joinable()) {
      worker_.request_stop();
      worker_.join();
    }

    CloseFile();
  }

  // Meyer's Singleton：函数局部静态变量保证线程安全且延迟初始化。
  static AsyncLoggerEngine& Instance() {
    static AsyncLoggerEngine engine;
    return engine;
  }

 private:
  // --------------------------------------------------------------------------
  // StartWorkerIfNeeded —— CAS 确保只有一个线程成功启动后台 worker
  // --------------------------------------------------------------------------
  void StartWorkerIfNeeded() {
    bool expected = false;
    if (!worker_started_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      return;
    }

    // std::jthread + stop_token 提供清晰的线程所有权与关闭语义。
    worker_ = std::jthread(
        [this](std::stop_token token) { WorkerLoop(token); });
  }

  // --------------------------------------------------------------------------
  // RegisterAtExit —— 全局注册一次 atexit 回调，进程退出时自动 Shutdown
  // --------------------------------------------------------------------------
  void RegisterAtExit() {
    bool expected = false;
    if (g_atexit_registered.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
      std::atexit([] { AsyncLoggerEngine::Instance().Shutdown(); });
    }
  }

  // --------------------------------------------------------------------------
  // ResolveLogPath —— 按优先级解析日志文件路径
  // 优先级：options.file_path > options.log_file_path > 默认路径
  // --------------------------------------------------------------------------
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

  // 线程安全地读取当前待生效的日志路径。
  [[nodiscard]] std::string CurrentLogPath() {
    std::lock_guard<std::mutex> lock(path_mu_);
    return pending_log_path_;
  }

  // ==========================================================================
  // WorkerLoop —— 后台消费线程主循环
  // ==========================================================================
  // 每次循环执行：
  //   1. 检查 reopen_requested_，若被置位则重新打开文件。
  //   2. 批量弹出日志（PopBatch）。
  //   3. 若有日志：格式化整批 → 写入文件 → 按需 flush → 输出丢弃摘要。
  //   4. 若无日志：检查 flush 请求、检查 stop_token、自旋/休眠等待。
  // 退出前（stop requested）会尽力排空队列中剩余日志，保证不丢失已入队数据。
  // ==========================================================================
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
      }

      EmitDropSummaryIfNeeded(false, last_drop_report);

      const auto now = std::chrono::steady_clock::now();
      if (flush_requested_.exchange(false, std::memory_order_acq_rel) ||
          now - last_flush >= FlushInterval()) {
        FlushFile();
        last_flush = now;
      }

      if (drained == 0) {
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
          const auto sleep_us = idle_sleep_us_.load(std::memory_order_relaxed);
          if (sleep_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
          }
        }
      }
    }

    // 退出前尽力排空队列：保证 Shutdown 前已入队日志全部落盘。
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

  // --------------------------------------------------------------------------
  // EstimateFormattedBatchBytes —— 预估格式化后字符串总长度，用于 reserve 减少重分配
  // --------------------------------------------------------------------------
  [[nodiscard]] std::size_t EstimateFormattedBatchBytes(
      std::span<const LogEntry> batch) const noexcept {
    constexpr std::size_t kBasePerEntry = 48;
    std::size_t total = 0;

    for (const auto& entry : batch) {
      total += kBasePerEntry;
      total += entry.tid_size;
      total += std::char_traits<char>::length(entry.file_name);
      total += std::char_traits<char>::length(entry.function_name);
      total += entry.message.View().size();
      total += 12;  // 行号数字 + 分隔符/换行符余量
    }

    total += batch.size() * 8;
    return total;
  }

  // --------------------------------------------------------------------------
  // FormatBatch —— 将一批 LogEntry 格式化为单个大字符串
  // --------------------------------------------------------------------------
  void FormatBatch(std::span<const LogEntry> batch, std::string& out) {
    out.clear();
    const std::size_t estimate = EstimateFormattedBatchBytes(batch);
    if (out.capacity() < estimate) {
      out.reserve(estimate);
    }

    for (const auto& entry : batch) {
      AppendFormattedEntry(entry, out);
    }
  }

  // --------------------------------------------------------------------------
  // AppendFormattedEntry —— 单条日志格式化："[YYYY-MM-DD HH:MM:SS.mmm][LEVEL][tid=...] file:line func | msg\n"
  // --------------------------------------------------------------------------
  // 优化点：秒级缓存 cached_second_text_，避免每条日志都调用 strftime。
  // 只有秒数变化时才重新生成 "YYYY-MM-DD HH:MM:SS" 部分；毫秒部分单独计算。
  // --------------------------------------------------------------------------
  void AppendFormattedEntry(const LogEntry& entry, std::string& out) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        entry.timestamp.time_since_epoch()) %
                    1000;
    const std::time_t sec = std::chrono::system_clock::to_time_t(entry.timestamp);

    if (sec != cached_second_) {
      std::tm local_tm{};
#if defined(_WIN32)
      localtime_s(&local_tm, &sec);
#else
      localtime_r(&sec, &local_tm);  // 线程安全的本地时间转换
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

  // --------------------------------------------------------------------------
  // WriteBuffer —— 将格式化后的字符串写入文件，失败时降级到 stderr
  // --------------------------------------------------------------------------
  void WriteBuffer(std::string_view bytes) noexcept {
    if (bytes.empty()) {
      return;
    }

    if (file_ != nullptr) {
      const std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file_);
      if (written == bytes.size()) {
        return;
      }
      // 部分写入：将剩余内容输出到 stderr 作为降级。
      WriteFallback(bytes.substr(written));
      return;
    }

    WriteFallback(bytes);
  }

  // --------------------------------------------------------------------------
  // EmitDropSummaryIfNeeded —— 周期性输出丢弃摘要日志
  // --------------------------------------------------------------------------
  // 当存在被丢弃的日志时，每隔 kDropSummaryInterval 输出一条 Warn 级别摘要，
  // 提醒用户队列已满载。force_emit = true 时立即输出（用于 Shutdown 前的最终汇报）。
  // --------------------------------------------------------------------------
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

  // --------------------------------------------------------------------------
  // FlushInterval —— 读取当前配置的自动 flush 间隔（毫秒）
  // --------------------------------------------------------------------------
  [[nodiscard]] std::chrono::milliseconds FlushInterval() const noexcept {
    return std::chrono::milliseconds(
        flush_interval_ms_.load(std::memory_order_acquire));
  }

  // --------------------------------------------------------------------------
  // OpenFile —— 以追加模式（"ab"）打开日志文件，并设置 1 MB 全缓冲
  // --------------------------------------------------------------------------
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

    // _IOFBF：全缓冲，缓冲区大小 1 MB，减少 syscall 次数。
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

  // --------------------------------------------------------------------------
  // WriteFallback —— 当文件不可用时，将日志输出到 stderr
  // --------------------------------------------------------------------------
  void WriteFallback(std::string_view bytes) noexcept {
    if (bytes.empty()) {
      return;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), stderr);
  }

  // ==========================================================================
  // 成员变量
  // ==========================================================================

  // 无锁 MPSC 环形队列，容量固定为 kRingCapacity。
  MpscRingQueue<LogEntry, kRingCapacity> queue_;

  // --- 运行时参数（原子化，支持生产者线程安全地读取/更新） ---
  std::atomic<std::size_t> inflight_count_{0};               // 当前在队列中的日志数
  std::atomic<std::size_t> queue_capacity_soft_{kDefaultQueueCapacity}; // 软上限
  std::atomic<std::size_t> max_batch_size_{kDefaultBatchSize};          // 批量消费大小
  std::atomic<std::int64_t> flush_interval_ms_{kDefaultFlushInterval.count()};
  std::atomic<std::uint32_t> spin_before_sleep_{128};        // 休眠前自旋次数
  std::atomic<std::int64_t> idle_sleep_us_{kDefaultIdleSleep.count()};  // 空闲休眠时长（微秒）
  std::atomic<std::size_t> inline_message_threshold_{kDefaultInlineMessageCapacity};

  std::atomic<DropPolicy> drop_policy_{DropPolicy::DropNewest};
  std::atomic<LogLevel> min_level_{LogLevel::kInfo};

  // --- 控制标志 ---
  std::atomic<bool> accepting_logs_{true};   // 是否仍接收新日志（Shutdown 后置 false）
  std::atomic<bool> worker_started_{false};  // worker 线程是否已启动
  std::atomic<bool> flush_requested_{false}; // 是否有外部 flush 请求
  std::atomic<bool> reopen_requested_{false};// 是否需要重新打开文件

  // --- 统计计数器 ---
  std::atomic<std::uint64_t> submit_calls_{0};       // 总 Submit 调用次数
  std::atomic<std::uint64_t> filtered_by_level_{0};  // 因级别过滤而丢弃的数量
  std::atomic<std::uint64_t> enqueued_{0};           // 成功入队数量
  std::atomic<std::uint64_t> consumed_{0};           // 成功消费数量
  std::atomic<std::uint64_t> dropped_{0};            // 总丢弃数量
  std::atomic<std::uint64_t> dropped_for_summary_{0};// 待输出摘要的丢弃数量

  // 非原子：仅在 worker 线程内访问，用于累积丢弃摘要。
  std::uint64_t pending_drop_summary_{0};

  // 日志路径保护：Init/SetLogFile 在生产者线程更新，worker 线程读取，需加锁。
  std::mutex path_mu_;
  std::string pending_log_path_{std::string(kDefaultLogFilePath)};

  std::FILE* file_{nullptr};            // 当前日志文件句柄
  std::jthread worker_;                 // 后台消费线程（C++20 jthread）

  std::string formatted_batch_buffer_;  // 批量格式化结果的复用缓冲区

  // 秒级缓存：cached_second_ 记录上一次格式化的秒级时间戳，避免重复 strftime。
  std::time_t cached_second_{static_cast<std::time_t>(-1)};
  std::array<char, 32> cached_second_text_{};
};

}  // namespace

// ============================================================================
// 公共 API 实现 —— 对外暴露的便捷接口，均委托给 AsyncLoggerEngine 单例
// ============================================================================

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
