#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/unique_fd.h"
#include "server/worker_loop.h"

namespace rpc::server {

namespace {

constexpr int kAcceptPollTimeoutMs = 100;

WorkerLoop& SelectWorker(std::vector<WorkerLoop>& workers,
                         std::size_t* next_worker) {
  WorkerLoop& selected = workers[*next_worker];
  *next_worker = (*next_worker + 1) % workers.size();
  return selected;
}

}  // namespace

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry)
    : port_(port), registry_(registry) {}

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

  // 当前仅运行一个 worker，但通过容器形式保留未来扩展点。
  std::vector<WorkerLoop> workers;
  workers.emplace_back(0U, registry_);

  std::string worker_error;
  if (!workers[0].Init(&worker_error)) {
    common::LogError("worker init failed: " + worker_error);
    return false;
  }

  common::LogInfo("RPC server listening on port " + std::to_string(port_) +
                  ", workers=1");

  epoll_event events[16] = {};
  std::size_t next_worker = 0;

  while (true) {
    const int ready =
        ::epoll_wait(accept_epoll_fd.Get(), events, 16, kAcceptPollTimeoutMs);
    if (ready < 0) {
      if (errno != EINTR) {
        common::LogError(std::string("accept epoll_wait failed: ") +
                         std::strerror(errno));
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
          if (!worker.AddConnection(common::UniqueFd(client_raw_fd), peer,
                                    &add_error)) {
            common::LogError("worker add connection failed: " + add_error);
            ::close(client_raw_fd);
          }
        }
      }
    }

    for (auto& worker : workers) {
      std::string poll_error;
      if (!worker.PollOnce(0, &poll_error)) {
        common::LogError("worker poll failed: " + poll_error);
        return false;
      }
    }
  }
}

}  // namespace rpc::server
