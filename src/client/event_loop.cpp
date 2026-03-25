#include "client/event_loop.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "common/log.h"

namespace rpc::client {

namespace {

// 区分 wake_fd 事件与业务 socket 事件的固定标识。
constexpr std::uint64_t kWakeToken = 1;

}  // namespace

EventLoop::EventLoop() = default;

bool EventLoop::Init() {
  // 创建 epoll 实例，作为事件复用器。
  epoll_fd_.Reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd_) {
    common::LogError(std::string("epoll_create1 failed: ") +
                     std::strerror(errno));
    return false;
  }

  // eventfd 作为“自唤醒管道”：Close 可通过它中断 epoll_wait。
  wake_fd_.Reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_fd_) {
    common::LogError(std::string("eventfd failed: ") + std::strerror(errno));
    return false;
  }

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeToken;
  // 将 wake_fd 注册进 epoll，用于接收 Wakeup() 信号。
  if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_ADD, wake_fd_.Get(), &ev) < 0) {
    common::LogError(std::string("epoll_ctl add wake_fd failed: ") +
                     std::strerror(errno));
    return false;
  }

  return true;
}

bool EventLoop::SetReadFd(int fd) {
  if (!epoll_fd_) {
    return false;
  }

  epoll_event ev{};
  // 关注可读和对端关闭相关事件，便于上层统一收敛连接状态。
  ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_.Get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
    common::LogError(std::string("epoll_ctl add read fd failed: ") +
                     std::strerror(errno));
    return false;
  }

  read_fd_ = fd;
  return true;
}

EventLoop::WaitResult EventLoop::WaitOnce(int timeout_ms) const {
  if (!epoll_fd_ || read_fd_ < 0) {
    return WaitResult::kError;
  }

  epoll_event ev{};
  const int n = ::epoll_wait(epoll_fd_.Get(), &ev, 1, timeout_ms);
  if (n == 0) {
    // 让上层借此执行周期性超时扫描（FailTimedOut）。
    return WaitResult::kTimeout;
  }
  if (n < 0) {
    if (errno == EINTR) {
      return WaitResult::kTimeout;
    }
    return WaitResult::kError;
  }

  if (ev.data.u64 == kWakeToken) {
    // 清空 eventfd 计数，避免重复触发。
    std::uint64_t value = 0;
    (void)::read(wake_fd_.Get(), &value, sizeof(value));
    return WaitResult::kWakeup;
  }

  return WaitResult::kReadable;
}

void EventLoop::Wakeup() const {
  if (!wake_fd_) {
    return;
  }

  const std::uint64_t value = 1;
  // 写入 eventfd 触发 EPOLLIN，从而唤醒正在等待的线程。
  (void)::write(wake_fd_.Get(), &value, sizeof(value));
}

}  // namespace rpc::client
