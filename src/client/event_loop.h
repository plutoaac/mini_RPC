#pragma once

#include <cstdint>

#include "common/unique_fd.h"

namespace rpc::client {

// 轻量单线程事件循环，仅用于等待一个连接 fd 的可读事件。
class EventLoop {
 public:
  // 单次等待结果：可读、超时、被主动唤醒、内部错误。
  enum class WaitResult {
    kReadable,
    kTimeout,
    kWakeup,
    kError,
  };

  EventLoop();

  // 初始化 epoll 与 wakeup fd。
  [[nodiscard]] bool Init();
  // 注册业务连接 fd 的可读事件。
  [[nodiscard]] bool SetReadFd(int fd);
  // 执行一次事件等待（不会循环），由上层控制调度节奏。
  [[nodiscard]] WaitResult WaitOnce(int timeout_ms) const;
  // 从其他线程唤醒 epoll_wait（例如 Close 时快速退出）。
  void Wakeup() const;

 private:
  // epoll 实例。
  rpc::common::UniqueFd epoll_fd_;
  // eventfd 用于线程间唤醒，避免等待超时才退出。
  rpc::common::UniqueFd wake_fd_;
  // 当前被监听的业务连接 fd。
  int read_fd_{-1};
};

}  // namespace rpc::client
