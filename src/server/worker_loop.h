#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

#include "common/unique_fd.h"
#include "coroutine/task.h"
#include "server/connection.h"
#include "server/service_registry.h"

namespace rpc::server {

// WorkerLoop 负责连接所属 epoll 循环和连接协程驱动。
// 当前版本采用 one-loop-per-thread：每个 WorkerLoop 运行在独立线程。
class WorkerLoop {
 public:
  // 析构时尝试停止并回收线程，避免后台线程泄漏。
  ~WorkerLoop();

  WorkerLoop(std::size_t worker_id, const ServiceRegistry& registry);

  // Init 只做 fd/epoll 资源初始化，不启动线程。
  bool Init(std::string* error_msg);
  // Start 在独立线程中运行 Run() 主循环。
  bool Start(std::string* error_msg);
  // RequestStop + Join 构成最小线程生命周期控制。
  void RequestStop();
  void Join();

  // 线程安全：供 acceptor 线程投递新连接，真实接管在 worker 线程完成。
  bool EnqueueConnection(rpc::common::UniqueFd fd, std::string peer_desc,
                         std::string* error_msg);

  bool AddConnection(rpc::common::UniqueFd fd, std::string_view peer_desc,
                     std::string* error_msg);
  bool PollOnce(int timeout_ms, std::string* error_msg);

  [[nodiscard]] std::size_t WorkerId() const noexcept;
  [[nodiscard]] std::size_t ConnectionCount() const noexcept;
  // 总接管连接数（单调递增），用于结构性验证分发是否生效。
  [[nodiscard]] std::size_t TotalAcceptedCount() const noexcept;
  [[nodiscard]] bool IsOnOwnerThread() const noexcept;

 private:
  // acceptor 线程投递到 worker 的最小数据单元。
  struct PendingConnection {
    rpc::common::UniqueFd fd;
    std::string peer_desc;
  };

  struct ConnectionState {
    ConnectionState(rpc::common::UniqueFd fd, const ServiceRegistry& registry)
        : connection(std::move(fd), registry) {}

    Connection connection;
    std::optional<rpc::coroutine::Task<void>> task;
    bool coroutine_ok{true};
    std::string coroutine_error;
  };

  // 仅允许在 worker owner 线程执行真实接管。
  bool AddConnectionOnOwnerThread(rpc::common::UniqueFd fd,
                                  std::string_view peer_desc,
                                  std::string* error_msg);
  // Drain 流程：先 drain wake fd，再 drain pending queue。
  bool DrainPendingConnections(std::string* error_msg);
  bool DrainWakeFd(std::string* error_msg);
  // 停止时收敛所有连接，确保协程退出和 fd 回收。
  void CloseAllConnections();
  // 跨线程唤醒 worker epoll_wait。
  void Wakeup();
  // worker 线程主循环。
  void Run();

  bool CloseConnection(int fd, std::string_view reason);
  bool EnsureOwnerThread(std::string* error_msg) const;

  std::size_t worker_id_;
  const ServiceRegistry& registry_;
  rpc::common::UniqueFd epoll_fd_;
  // wake_fd_ 用于接收 EnqueueConnection/RequestStop 的跨线程唤醒信号。
  rpc::common::UniqueFd wake_fd_;
  std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;

  // acceptor -> worker 的跨线程移交队列。
  std::mutex pending_mutex_;
  std::deque<PendingConnection> pending_connections_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<std::size_t> connection_count_{0};
  std::atomic<std::size_t> total_accepted_count_{0};

  std::thread thread_;
  std::thread::id owner_thread_id_{};
};

}  // namespace rpc::server
