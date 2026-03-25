#include "client/event_loop.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

void TestReadableEvent() {
  int sv[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  rpc::client::EventLoop loop;
  assert(loop.Init());
  assert(loop.SetReadFd(sv[0]));

  const char c = 'x';
  assert(::write(sv[1], &c, 1) == 1);

  const auto result = loop.WaitOnce(200);
  assert(result == rpc::client::EventLoop::WaitResult::kReadable);

  char out = 0;
  assert(::read(sv[0], &out, 1) == 1);
  assert(out == c);

  ::close(sv[0]);
  ::close(sv[1]);
}

void TestTimeoutEvent() {
  int sv[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  rpc::client::EventLoop loop;
  assert(loop.Init());
  assert(loop.SetReadFd(sv[0]));

  const auto result = loop.WaitOnce(30);
  assert(result == rpc::client::EventLoop::WaitResult::kTimeout);

  ::close(sv[0]);
  ::close(sv[1]);
}

void TestWakeupEvent() {
  int sv[2] = {-1, -1};
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

  rpc::client::EventLoop loop;
  assert(loop.Init());
  assert(loop.SetReadFd(sv[0]));

  std::thread notifier([&loop]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loop.Wakeup();
  });

  const auto result = loop.WaitOnce(300);
  assert(result == rpc::client::EventLoop::WaitResult::kWakeup);

  notifier.join();
  ::close(sv[0]);
  ::close(sv[1]);
}

}  // namespace

int main() {
  TestReadableEvent();
  TestTimeoutEvent();
  TestWakeupEvent();

  std::cout << "event_loop_test passed\n";
  return 0;
}
