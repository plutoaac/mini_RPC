#include "common/log.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace rpc::common {
namespace {

constexpr std::size_t kMaxQueueSize = 4096;
constexpr std::chrono::milliseconds kWaitTimeout{200};
constexpr std::chrono::seconds kFlushInterval{1};
constexpr const char* kDefaultLogPath = "./rpc.log";

[[nodiscard]] std::string NowTimestamp() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const std::time_t t = clock::to_time_t(now);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) %
                  1000;

  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);

  char base_time[32] = {0};
  if (std::strftime(base_time, sizeof(base_time), "%Y-%m-%d %H:%M:%S",
                    &tm_buf) == 0) {
    return "1970-01-01 00:00:00.000";
  }

  char out[40] = {0};
  std::snprintf(out, sizeof(out), "%s.%03d", base_time,
                static_cast<int>(ms.count()));
  return out;
}

[[nodiscard]] std::string ThreadIdString() {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  return oss.str();
}

[[nodiscard]] std::string FormatLogLine(LogLevel level,
                                        std::string_view message,
                                        const std::source_location& location) {
  std::ostringstream oss;
  oss << '[' << LogLevelName(level) << "] " << NowTimestamp()
      << " tid=" << ThreadIdString() << ' ' << Basename(location.file_name())
      << ':' << location.line() << ' ' << location.function_name() << " | "
      << message;
  return oss.str();
}

class AsyncFileLogger final {
 public:
  AsyncFileLogger() : log_path_(kDefaultLogPath) {
    OpenLogFile(log_path_);
    worker_ = std::thread([this] { WorkerLoop(); });
  }

  ~AsyncFileLogger() { Shutdown(); }

  void Enqueue(std::string line) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stopped_) {
        return;
      }
      if (queue_.size() >= kMaxQueueSize) {
        queue_.pop_front();
        ++dropped_count_;
      }
      queue_.push_back(std::move(line));
    }
    cv_.notify_one();
  }

  void SetLogFilePath(std::string path) {
    if (path.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      log_path_ = std::move(path);
      reopen_requested_ = true;
    }
    cv_.notify_one();
  }

  void Flush() {
    std::lock_guard<std::mutex> lock(file_mu_);
    if (file_.is_open()) {
      file_.flush();
    }
  }

  void Shutdown() {
    bool need_join = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!stopped_) {
        stopped_ = true;
        need_join = true;
      }
    }
    cv_.notify_one();

    if (need_join && worker_.joinable()) {
      worker_.join();
    }

    std::lock_guard<std::mutex> lock(file_mu_);
    if (file_.is_open()) {
      file_.flush();
      file_.close();
    }
  }

 private:
  void WorkerLoop() {
    auto last_flush = std::chrono::steady_clock::now();

    while (true) {
      std::deque<std::string> batch;
      std::size_t dropped_snapshot = 0;
      bool should_stop = false;

      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait_for(lock, kWaitTimeout, [this] {
          return stopped_ || reopen_requested_ || !queue_.empty();
        });

        if (reopen_requested_) {
          const std::string path = log_path_;
          reopen_requested_ = false;
          lock.unlock();
          OpenLogFile(path);
          lock.lock();
        }

        if (!queue_.empty()) {
          batch.swap(queue_);
        }
        dropped_snapshot = dropped_count_;
        dropped_count_ = 0;
        should_stop = stopped_ && queue_.empty();
      }

      if (!batch.empty()) {
        WriteBatch(batch);
      }

      if (dropped_snapshot > 0) {
        WriteLine("[WARN] " + NowTimestamp() + " tid=" + ThreadIdString() +
                  " logger 0 logger | dropped " +
                  std::to_string(dropped_snapshot) + " log messages");
      }

      const auto now = std::chrono::steady_clock::now();
      if (now - last_flush >= kFlushInterval) {
        Flush();
        last_flush = now;
      }

      if (should_stop) {
        Flush();
        break;
      }
    }
  }

  void OpenLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(file_mu_);
    if (file_.is_open()) {
      file_.flush();
      file_.close();
    }

    file_.open(path, std::ios::out | std::ios::app);
    file_ok_ = file_.is_open();
    if (!file_ok_) {
      std::cerr << "[ERROR] " << NowTimestamp() << " tid=" << ThreadIdString()
                << " logger 0 open | failed to open log file: " << path << '\n';
    }
  }

  void WriteBatch(const std::deque<std::string>& batch) {
    std::lock_guard<std::mutex> lock(file_mu_);
    if (file_ok_) {
      for (const auto& line : batch) {
        file_ << line << '\n';
      }
      return;
    }

    for (const auto& line : batch) {
      std::cerr << line << '\n';
    }
  }

  void WriteLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(file_mu_);
    if (file_ok_) {
      file_ << line << '\n';
      return;
    }
    std::cerr << line << '\n';
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;

  std::mutex file_mu_;
  std::ofstream file_;
  std::string log_path_;

  std::thread worker_;
  bool stopped_{false};
  bool reopen_requested_{false};
  bool file_ok_{false};
  std::size_t dropped_count_{0};
};

AsyncFileLogger& GetLogger() {
  static AsyncFileLogger logger;
  return logger;
}

}  // namespace

void SetLogFile(std::string_view path) {
  GetLogger().SetLogFilePath(std::string(path));
}

void FlushLogger() { GetLogger().Flush(); }

void ShutdownLogger() { GetLogger().Shutdown(); }

void StopLogger() { ShutdownLogger(); }

void Log(LogLevel level, std::string_view message,
         const std::source_location& location) {
  GetLogger().Enqueue(FormatLogLine(level, message, location));
}

}  // namespace rpc::common