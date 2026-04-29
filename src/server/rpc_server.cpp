#include "server/rpc_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

#include "common/log.h"
#include "common/unique_fd.h"
#include "server/worker_loop.h"

namespace rpc::server {

namespace {

constexpr int kAcceptPollTimeoutMs = 100;

}  // namespace

RpcServer::RpcServer(std::uint16_t port, const ServiceRegistry& registry,
                     std::size_t worker_count,
                     std::size_t business_thread_count)
    : port_(port),
      worker_count_(worker_count == 0 ? 1 : worker_count),
  business_thread_count_(business_thread_count),
      registry_(registry) {}

RpcServer::~RpcServer() { (void)Stop(); }

bool RpcServer::InitAcceptor(std::string* error_msg) {
  listen_fd_ = common::UniqueFd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!listen_fd_) {
    if (error_msg != nullptr) {
      *error_msg = std::string("socket failed: ") + std::strerror(errno);
    }
    return false;
  }

  int reuse = 1;
  if (::setsockopt(listen_fd_.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("setsockopt(SO_REUSEADDR) failed: ") +
                   std::strerror(errno);
    }
    return false;
  }

#ifdef SO_REUSEPORT
  if (::setsockopt(listen_fd_.Get(), SOL_SOCKET, SO_REUSEPORT, &reuse,
                   sizeof(reuse)) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("setsockopt(SO_REUSEPORT) failed: ") +
                   std::strerror(errno);
    }
    return false;
  }
#endif

  const int listen_flags = ::fcntl(listen_fd_.Get(), F_GETFL, 0);
  if (listen_flags < 0 ||
      ::fcntl(listen_fd_.Get(), F_SETFL, listen_flags | O_NONBLOCK) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("fcntl(O_NONBLOCK) failed for listen fd: ") +
                   std::strerror(errno);
    }
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (::bind(listen_fd_.Get(), reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("bind failed: ") + std::strerror(errno);
    }
    return false;
  }

  if (::listen(listen_fd_.Get(), 128) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("listen failed: ") + std::strerror(errno);
    }
    return false;
  }

  accept_epoll_fd_ = common::UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  if (!accept_epoll_fd_) {
    if (error_msg != nullptr) {
      *error_msg = std::string("epoll_create1 failed: ") + std::strerror(errno);
    }
    return false;
  }

  epoll_event listen_ev{};
  listen_ev.events = EPOLLIN;
  listen_ev.data.fd = listen_fd_.Get();
  if (::epoll_ctl(accept_epoll_fd_.Get(), EPOLL_CTL_ADD, listen_fd_.Get(),
                  &listen_ev) < 0) {
    if (error_msg != nullptr) {
      *error_msg = std::string("epoll_ctl add listen fd failed: ") +
                   std::strerror(errno);
    }
    return false;
  }

  return true;
}

bool RpcServer::StartBusinessThreadPool(std::string* error_msg) {
  if (business_thread_count_ == 0) {
    business_thread_pool_.reset();
    return true;
  }

  business_thread_pool_ =
      std::make_unique<rpc::common::ThreadPool>(business_thread_count_);
  if (!business_thread_pool_->Start(error_msg)) {
    business_thread_pool_.reset();
    return false;
  }
  return true;
}

bool RpcServer::StartWorkers(std::string* error_msg) {
  // 重新启动时先清空旧 worker，保证成员资源语义一致。
  workers_.clear();
  workers_.reserve(worker_count_);

  for (std::size_t i = 0; i < worker_count_; ++i) {
    auto worker =
        std::make_unique<WorkerLoop>(i, registry_, business_thread_pool_.get());
    // Init 负责 fd/epoll 基础资源；Start 才真正拉起线程。
    if (!worker->Init(error_msg)) {
      return false;
    }
    if (!worker->Start(error_msg)) {
      return false;
    }
    workers_.push_back(std::move(worker));
  }
  return true;
}

void RpcServer::StopBusinessThreadPool() {
  if (!business_thread_pool_) {
    return;
  }
  business_thread_pool_->Stop();
  business_thread_pool_->Join();
  business_thread_pool_.reset();
}

void RpcServer::StopWorkers() {
  // 第一阶段：并行广播停止请求，尽快阻止继续接管连接。
  for (auto& worker : workers_) {
    worker->RequestStop();
  }
  // 第二阶段：等待所有 worker 线程退出，形成确定性收敛点。
  for (auto& worker : workers_) {
    worker->Join();
  }
  workers_.clear();
}

void RpcServer::CloseAcceptor() {
  accept_epoll_fd_.Reset();
  listen_fd_.Reset();
}

WorkerLoop& RpcServer::SelectWorker() {
  // 轮询分发：简单、无锁、可预测，且与 one-loop-per-thread 模型对齐。
  // 这里由 acceptor 线程单线程访问，不需要额外同步。
  WorkerLoop& selected = *workers_[next_worker_];
  next_worker_ = (next_worker_ + 1) % workers_.size();
  return selected;
}

bool RpcServer::Start() {
  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    if (running_) {
      common::LogWarn(
          "RpcServer::Start called while server is already running");
      return false;
    }
    running_ = true;
  }

  // 发布运行态。Stop() 会通过这两个原子量触发退出路径。
  stop_requested_.store(false);
  accepting_new_connections_.store(true);
  next_worker_ = 0;

  // 阶段 1：建立 acceptor 资源（listen fd + epoll）。
  std::string init_error;
  if (!InitAcceptor(&init_error)) {
    common::LogError(init_error);
    CloseAcceptor();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    running_ = false;
    lifecycle_cv_.notify_all();
    return false;
  }

  // 阶段 2：拉起 worker 线程池（每 worker 一个 event loop 线程）。
  if (!StartBusinessThreadPool(&init_error)) {
    common::LogError("business thread pool start failed: " + init_error);
    CloseAcceptor();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    running_ = false;
    lifecycle_cv_.notify_all();
    return false;
  }

  // 阶段 3：拉起 worker 线程（每 worker 一个 event loop 线程）。
  if (!StartWorkers(&init_error)) {
    common::LogError("worker init/start failed: " + init_error);
    StopBusinessThreadPool();
    StopWorkers();
    CloseAcceptor();
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    running_ = false;
    lifecycle_cv_.notify_all();
    return false;
  }

  common::LogInfo("RPC server listening on port " + std::to_string(port_) +
                  ", workers=" + std::to_string(worker_count_) +
                  ", business_threads=" +
                  std::to_string(business_thread_count_));

  epoll_event events[16] = {};
  bool fatal_error = false;

  // 阶段 3：accept 主循环。职责仅限“接入 + 分发”，不直接处理连接 I/O。
  while (!stop_requested_.load()) {
    const int ready =
        ::epoll_wait(accept_epoll_fd_.Get(), events, 16, kAcceptPollTimeoutMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (!stop_requested_.load()) {
        common::LogError(std::string("accept epoll_wait failed: ") +
                         std::strerror(errno));
        fatal_error = true;
      }
      break;
    }

    if (stop_requested_.load() || !accepting_new_connections_.load()) {
      continue;
    }

    if (ready > 0) {
      for (int i = 0; i < ready; ++i) {
        if (events[i].data.fd != listen_fd_.Get()) {
          continue;
        }

        // ET-like drain 习惯：一次 wake 后尽量 accept 到
        // EAGAIN，减少下次唤醒开销。
        while (true) {
          if (!accepting_new_connections_.load()) {
            break;
          }

          sockaddr_in client_addr{};
          socklen_t client_addr_len = sizeof(client_addr);
          const int client_raw_fd = ::accept4(
              listen_fd_.Get(), reinterpret_cast<sockaddr*>(&client_addr),
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

          WorkerLoop& worker = SelectWorker();

          std::string add_error;
          // acceptor 线程只投递连接，真实接管(注册 epoll/启动协程)在 worker
          // 线程完成。
          if (!worker.EnqueueConnection(common::UniqueFd(client_raw_fd), peer,
                                        &add_error)) {
            common::LogError("worker add connection failed: " + add_error);
            ::close(client_raw_fd);
          }
        }
      }
    }
  }

  // 统一停机收敛：停止接入、回收 acceptor fd、停止并回收所有 worker。
  accepting_new_connections_.store(false);
  stop_requested_.store(true);
  CloseAcceptor();
  StopBusinessThreadPool();
  StopWorkers();

  {
    std::lock_guard<std::mutex> lock(lifecycle_mu_);
    running_ = false;
  }
  lifecycle_cv_.notify_all();

  return !fatal_error;
}

bool RpcServer::Stop() {
  {
    std::unique_lock<std::mutex> lock(lifecycle_mu_);
    if (!running_) {
      // 允许幂等 Stop，保证调用方无需关心当前状态。
      // 这里顺手执行一次兜底回收，覆盖 "Start 从未成功" 等边界场景。
      stop_requested_.store(true);
      accepting_new_connections_.store(false);
      StopBusinessThreadPool();
      StopWorkers();
      CloseAcceptor();
      return true;
    }
  }

  // 先发布停止请求并停止接入，实际资源回收由 Start 线程统一完成。
  accepting_new_connections_.store(false);
  stop_requested_.store(true);

  std::unique_lock<std::mutex> lock(lifecycle_mu_);
  // 等待 Start 完成统一清理并把 running_ 置回 false。
  lifecycle_cv_.wait(lock, [this]() { return !running_; });
  return true;
}

RpcServer::RuntimeStatsSnapshot RpcServer::StatsSnapshot() const {
  RuntimeStatsSnapshot snapshot;

  std::lock_guard<std::mutex> lock(lifecycle_mu_);
  snapshot.workers.reserve(workers_.size());
  for (const auto& worker : workers_) {
    WorkerRuntimeStats stats;
    stats.worker_id = worker->WorkerId();
    stats.current_connections = worker->ConnectionCount();
    stats.total_accepted_connections = worker->TotalAcceptedCount();
    stats.in_flight_requests = worker->InFlightRequestCount();
    stats.submitted_requests = worker->SubmittedRequestCount();
    stats.completed_requests = worker->CompletedRequestCount();
    const auto method_stats = worker->MethodStats();
    stats.method_stats.reserve(method_stats.size());
    for (const auto& method : method_stats) {
      stats.method_stats.push_back(MethodRuntimeStats{
          method.method, method.call_count, method.failure_count});
    }
    snapshot.workers.push_back(std::move(stats));
  }

  if (business_thread_pool_) {
    snapshot.business_thread_pool = business_thread_pool_->GetStatsSnapshot();
  }

  return snapshot;
}

}  // namespace rpc::server
