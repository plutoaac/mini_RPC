#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "common/unique_fd.h"
#include "coroutine/task.h"
#include "server/connection.h"
#include "server/service_registry.h"

namespace rpc::server {

// WorkerLoop 负责连接所属 epoll 循环和连接协程驱动。
// 当前仅单 worker 运行，但接口和职责已为多 worker 扩展预留。
class WorkerLoop {
 public:
  WorkerLoop(std::size_t worker_id, const ServiceRegistry& registry);

  bool Init(std::string* error_msg);
  bool AddConnection(rpc::common::UniqueFd fd, std::string_view peer_desc,
                     std::string* error_msg);
  bool PollOnce(int timeout_ms, std::string* error_msg);

  [[nodiscard]] std::size_t WorkerId() const noexcept;
  [[nodiscard]] std::size_t ConnectionCount() const noexcept;

 private:
  struct ConnectionState {
    ConnectionState(rpc::common::UniqueFd fd, const ServiceRegistry& registry)
        : connection(std::move(fd), registry) {}

    Connection connection;
    std::optional<rpc::coroutine::Task<void>> task;
    bool coroutine_ok{true};
    std::string coroutine_error;
  };

  bool CloseConnection(int fd, std::string_view reason);

  std::size_t worker_id_;
  const ServiceRegistry& registry_;
  rpc::common::UniqueFd epoll_fd_;
  std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;
};

}  // namespace rpc::server
