#pragma once

#include <unistd.h>

namespace rpc::common {

// POSIX 文件描述符的 RAII 封装：对象析构时自动 close，防止泄漏。
class UniqueFd {
 public:
  constexpr UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}

  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  [[nodiscard]] int Get() const noexcept { return fd_; }
  [[nodiscard]] bool Valid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return Valid(); }

  // 释放所有权但不关闭 fd，交给调用方后续管理。
  int Release() noexcept {
    const int old = fd_;
    fd_ = -1;
    return old;
  }

  // 重置到新 fd；若当前 fd 有效会先执行 close。
  void Reset(int new_fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = new_fd;
  }

 private:
  int fd_{-1};
};

}  // namespace rpc::common
