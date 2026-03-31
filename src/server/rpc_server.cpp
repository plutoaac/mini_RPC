#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/unique_fd.h"
#include "server/worker_loop.h"

namespace rpc::server {

namespace {

constexpr int kAcceptPollTimeoutMs = 100;

WorkerLoop& SelectWorker(
    std::vector<std::unique_ptr<WorkerLoop>>& workers,
                         std::size_t* next_worker) {
  // 最小分发策略：轮询选择 worker。
  WorkerLoop& selected = *workers[*next_worker];
  *next_worker = (*next_worker + 1) % workers.size();
  return selected;
}

}  // namespace

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry,
                     std::size_t worker_count)
    : port_(port), worker_count_(worker_count == 0 ? 1 : worker_count),
      registry_(registry) {}

bool RpcServer::Start() {
  common::UniqueFd listen_fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd) {
    common::LogError(std::string("socket failed: ") + std::strerror(errno));
    return false;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    common::LogError(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                     std::strerror(errno));
    return false;
  }

  const int listen_flags = ::fcntl(listen_fd.Get(), F_GETFL, 0);
  if (listen_flags < 0 ||
      ::fcntl(listen_fd.Get(), F_SETFL, listen_flags | O_NONBLOCK) < 0) {
    common::LogError(std::string("fcntl(O_NONBLOCK) failed for listen fd: ") +
                     std::strerror(errno));
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(listen_fd.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    common::LogError(std::string("bind failed: ") + std::strerror(errno));
    return false;
  }

  if (::listen(listen_fd.Get(), 128) < 0) {
    common::LogError(std::string("listen failed: ") + std::strerror(errno));
    return false;
  }

  common::UniqueFd accept_epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!accept_epoll_fd) {
    common::LogError(std::string("epoll_create1 failed: ") +
                     std::strerror(errno));
    return false;
  }

  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;
  listen_ev.data.fd = listen_fd.Get();
  if (::epoll_ctl(accept_epoll_fd.Get(), EPOLL_CTL_ADD, listen_fd.Get(),
                  &listen_ev) < 0) {
    common::LogError(std::string("epoll_ctl add listen fd failed: ") +
                     std::strerror(errno));
    return false;
  }

  std::vector<std::unique_ptr<WorkerLoop>> workers;
  workers.reserve(worker_count_);

  // 启动多个 WorkerLoop，每个 WorkerLoop 绑定独立线程运行。
  for (std::size_t i = 0; i < worker_count_; ++i) {
    auto worker = std::make_unique<WorkerLoop>(i, registry_);
    std::string worker_error;
    if (!worker->Init(&worker_error)) {
      common::LogError("worker init failed: " + worker_error);
      for (auto& started_worker : workers) {
        started_worker->RequestStop();
      }
      for (auto& started_worker : workers) {
        started_worker->Join();
      }
      return false;
    }
    if (!worker->Start(&worker_error)) {
      common::LogError("worker start failed: " + worker_error);
      for (auto& started_worker : workers) {
        started_worker->RequestStop();
      }
      for (auto& started_worker : workers) {
        started_worker->Join();
      }
      return false;
    }
    workers.push_back(std::move(worker));
  }

  auto stop_workers = [&workers]() {
    // 两阶段停止：先发 stop 信号，再 join，避免线程悬挂。
    for (auto& worker : workers) {
      worker->RequestStop();
    }
    for (auto& worker : workers) {
      worker->Join();
    }
  };

  common::LogInfo("RPC server listening on port " + std::to_string(port_) +
                  ", workers=" + std::to_string(worker_count_));

  epoll_event events[16] = {};
  std::size_t next_worker = 0;

  while (true) {
    const int ready =
        ::epoll_wait(accept_epoll_fd.Get(), events, 16, kAcceptPollTimeoutMs);
    if (ready < 0) {
      if (errno != EINTR) {
        common::LogError(std::string("accept epoll_wait failed: ") +
                         std::strerror(errno));
        stop_workers();
        return false;
      }
    }

    if (ready > 0) {
      for (int i = 0; i < ready; ++i) {
        if (events[i].data.fd != listen_fd.Get()) {
          continue;
        }

        while (true) {
          sockaddr_in client_addr{};
          socklen_t client_addr_len = sizeof(client_addr);
          const int client_raw_fd = ::accept4(
              listen_fd.Get(), reinterpret_cast<sockaddr*>(&client_addr),
              &client_addr_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client_raw_fd < 0) {
            if (errno == EINTR) {
              continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              break;
            }
            common::LogError(std::string("accept4 failed: ") +
                             std::strerror(errno));
            break;
          }

          char ip[INET_ADDRSTRLEN] = {0};
          ::inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
          const std::string peer = std::string(ip) + ':' +
                                   std::to_string(ntohs(client_addr.sin_port));

          WorkerLoop& worker = SelectWorker(workers, &next_worker);

          std::string add_error;
          // acceptor 线程只投递连接，真实接管在 worker 线程完成。
          if (!worker.EnqueueConnection(common::UniqueFd(client_raw_fd), peer,
                                        &add_error)) {
            common::LogError("worker add connection failed: " + add_error);
            ::close(client_raw_fd);
          }
        }
      }
    }
  }
}

}  // namespace rpc::server
