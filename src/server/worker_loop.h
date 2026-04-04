#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/thread_pool.h"
#include "common/unique_fd.h"
#include "coroutine/task.h"
#include "server/connection.h"
#include "server/service_registry.h"

namespace rpc::server {

// ============================================================================
// WorkerLoop - 多线程 RPC 服务器的工作线程事件循环
// ============================================================================
//
// WorkerLoop 负责连接所属 epoll 循环和连接协程驱动。
// 当前版本采用 one-loop-per-thread：每个 WorkerLoop 运行在独立线程。
//
// ## 架构概述
//
// ```
//   ┌─────────────────────────────────────────────────────────────┐
//   │                      Main Thread (Acceptor)                 │
//   │  listen_fd ──> accept() ──> EnqueueConnection() 分发给 Worker │
//   └─────────────────────────────────────────────────────────────┘
//                              │
//          ┌───────────────────┼───────────────────┐
//          ▼                   ▼                   ▼
//   ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
//   │ WorkerLoop 0│     │ WorkerLoop 1│     │ WorkerLoop N│
//   │  epoll +    │     │  epoll +    │     │  epoll +    │
//   │  协程       │     │  协程       │     │  协程       │
//   └─────────────┘     └─────────────┘     └─────────────┘
// ```
//
// ## 线程模型
//
// - 主线程负责 accept 新连接，通过 EnqueueConnection() 分发给 WorkerLoop
// - 每个 WorkerLoop 在独立线程中运行 epoll_wait + 协程调度
// - 可选配置线程池，将耗时业务逻辑从 I/O 线程卸载
//
class WorkerLoop {
 public:
  // ==========================================================================
  // 方法调用统计快照
  // ==========================================================================

  /**
   * @brief 方法调用统计快照
   *
   * 用于获取当前时刻各方法的调用统计信息，供监控和分析使用。
   */
  struct MethodStatsSnapshot {
    std::string method;          // 方法名称，格式为 "Service.Method"
    std::size_t call_count{0};   // 总调用次数
    std::size_t failure_count{0}; // 失败调用次数
  };

  // ==========================================================================
  // 构造函数和生命周期控制
  // ==========================================================================

  /**
   * @brief 析构函数
   *
   * 析构时尝试停止并回收线程，避免后台线程泄漏。
   */
  ~WorkerLoop();

  /**
   * @brief 构造函数
   *
   * @param worker_id Worker 标识，用于日志和调试
   * @param registry 服务注册表引用，用于查找 RPC 方法处理器
   * @param thread_pool 可选的线程池指针，配置后将异步执行业务逻辑
   */
  WorkerLoop(std::size_t worker_id, const ServiceRegistry& registry,
             rpc::common::ThreadPool* thread_pool = nullptr);

  /**
   * @brief 初始化 epoll 和 eventfd 资源
   *
   * Init 只做 fd/epoll 资源初始化，不启动线程。
   * 必须在 Start() 之前调用。
   *
   * @param error_msg 错误信息输出参数
   * @return true 初始化成功
   * @return false 初始化失败
   */
  bool Init(std::string* error_msg);

  /**
   * @brief 启动工作线程
   *
   * Start 在独立线程中运行 Run() 主循环。
   *
   * @param error_msg 错误信息输出参数
   * @return true 启动成功
   * @return false 启动失败（如未初始化或已启动）
   */
  bool Start(std::string* error_msg);

  /**
   * @brief 请求停止工作线程
   *
   * 设置停止标志并唤醒 epoll_wait，使工作线程退出主循环。
   * 非阻塞，需要配合 Join() 等待线程真正退出。
   */
  void RequestStop();

  /**
   * @brief 等待工作线程退出
   *
   * 阻塞直到工作线程执行完毕。
   * RequestStop + Join 构成最小线程生命周期控制。
   */
  void Join();

  // ==========================================================================
  // 连接管理
  // ==========================================================================

  /**
   * @brief 投递新连接到 worker（线程安全）
   *
   * 供 acceptor 线程投递新连接，真实接管在 worker 线程完成。
   * 连接会被加入待处理队列，通过 eventfd 唤醒 worker 线程处理。
   *
   * @param fd 客户端文件描述符
   * @param peer_desc 对端描述（如 IP:Port），用于日志
   * @param error_msg 错误信息输出参数
   * @return true 投递成功
   * @return false 投递失败（如 worker 正在停止）
   */
  bool EnqueueConnection(rpc::common::UniqueFd fd, std::string peer_desc,
                         std::string* error_msg);

  /**
   * @brief 直接添加连接（仅限 owner 线程调用）
   *
   * 在 owner 线程中直接将连接加入 epoll 管理。
   *
   * @param fd 客户端文件描述符
   * @param peer_desc 对端描述
   * @param error_msg 错误信息输出参数
   * @return true 添加成功
   * @return false 添加失败
   */
  bool AddConnection(rpc::common::UniqueFd fd, std::string_view peer_desc,
                     std::string* error_msg);

  /**
   * @brief 执行一次 epoll_wait 和事件处理
   *
   * 用于测试或集成到外部事件循环。
   *
   * @param timeout_ms epoll_wait 超时时间（毫秒）
   * @param error_msg 错误信息输出参数
   * @return true 处理成功
   * @return false 处理过程中发生错误
   */
  bool PollOnce(int timeout_ms, std::string* error_msg);

  // ==========================================================================
  // 状态查询方法
  // ==========================================================================

  /** @brief 获取 Worker ID */
  [[nodiscard]] std::size_t WorkerId() const noexcept;

  /** @brief 获取当前活跃连接数 */
  [[nodiscard]] std::size_t ConnectionCount() const noexcept;

  /**
   * @brief 获取总接管连接数（单调递增）
   *
   * 用于结构性验证分发是否生效。
   */
  [[nodiscard]] std::size_t TotalAcceptedCount() const noexcept;

  /**
   * @brief 获取正在处理中的请求数
   *
   * 仅在线程池模式下有意义，表示已投递到线程池但尚未完成的请求数。
   */
  [[nodiscard]] std::size_t InFlightRequestCount() const noexcept;

  /** @brief 获取已提交到线程池的请求总数 */
  [[nodiscard]] std::size_t SubmittedRequestCount() const noexcept;

  /** @brief 获取已完成的请求总数 */
  [[nodiscard]] std::size_t CompletedRequestCount() const noexcept;

  /** @brief 获取各方法的调用统计快照 */
  [[nodiscard]] std::vector<MethodStatsSnapshot> MethodStats() const;

  /** @brief 检查是否正在接受新连接 */
  [[nodiscard]] bool IsAcceptingNewConnections() const noexcept;

  /** @brief 检查当前是否在 owner 线程 */
  [[nodiscard]] bool IsOnOwnerThread() const noexcept;

 private:
  // ==========================================================================
  // 内部数据结构
  // ==========================================================================

  /**
   * @brief 待处理连接数据单元
   *
   * acceptor 线程投递到 worker 的最小数据单元。
   * 用于跨线程传递新连接信息。
   */
  struct PendingConnection {
    rpc::common::UniqueFd fd;  // 客户端文件描述符
    std::string peer_desc;      // 对端描述（IP:Port）
  };

  /**
   * @brief 连接状态结构体
   *
   * 管理单个客户端连接的完整生命周期状态，包括连接对象、协程任务和错误信息。
   * 每个 fd 对应一个 ConnectionState 实例，存储在 connections_ map 中。
   */
  struct ConnectionState {
    // 构造函数：接收已接管的文件描述符和服务注册表引用
    ConnectionState(rpc::common::UniqueFd fd, const ServiceRegistry& registry)
        : connection(std::move(fd), registry) {}

    Connection connection;                           // 连接对象，封装了 fd、读写缓冲区、请求序列号等
    std::uint64_t connection_token{0};               // 连接唯一标识令牌，用于验证请求归属，防止已关闭连接的迟到响应
    std::optional<rpc::coroutine::Task<void>> task;  // 连接主协程任务，驱动该连接上的请求处理流程
    bool coroutine_ok{true};                         // 协程执行状态标记，false 表示协程已异常退出
    std::string coroutine_error;                     // 协程错误信息，记录协程失败时的详细错误原因
  };

  /**
   * @brief 已完成响应结构体
   *
   * 表示一个已完成处理的 RPC 请求响应，由线程池工作线程产生，供 worker 线程发送给客户端。
   * 实现了请求处理与响应发送的解耦，支持异步非阻塞的响应流程。
   */
  struct CompletedResponse {
    int fd{-1};                             // 客户端连接的文件描述符，用于定位目标连接
    std::uint64_t connection_token{0};      // 连接令牌，用于验证响应归属，防止连接复用导致错发
    std::uint64_t request_sequence{0};      // 请求序列号（服务端生成，递增），用于调试请求处理顺序
    std::string request_id;                 // 请求唯一标识符（客户端生成），用于请求-响应匹配和日志追踪
    int proto_error_code{0};                // 协议层错误码，非零表示协议解析或处理错误
    std::string error_msg;                  // 错误消息，包含详细的错误描述信息
    std::string payload;                    // 响应负载数据，序列化后的响应内容
    std::string method_key;                 // 调用的方法键（"Service.Method"），用于方法调用统计
    bool handler_success{false};            // 业务处理器执行结果，true 表示业务逻辑成功执行
  };

  /**
   * @brief 已完成响应队列结构体
   *
   * 线程安全的响应队列，用于线程池工作线程向 worker 线程传递已完成的响应。
   * 实现了跨线程的生产者-消费者模式，配合 wake_fd 实现 epoll 驱动的响应发送。
   */
  struct CompletedQueue {
    std::mutex mutex;                        // 互斥锁，保护 responses 队列的并发访问
    std::deque<CompletedResponse> responses; // 已完成响应的双端队列
    std::atomic<bool> accepting{true};       // 队列接受状态标记，false 时拒绝新响应入队（用于优雅关闭）
    int wake_fd{-1};                         // 唤醒文件描述符，写入数据可唤醒 worker 线程的 epoll_wait
  };

  // ==========================================================================
  // 内部方法
  // ==========================================================================

  // 仅允许在 worker owner 线程执行真实接管。
  bool AddConnectionOnOwnerThread(rpc::common::UniqueFd fd,
                                  std::string_view peer_desc,
                                  std::string* error_msg);

  // 连接进入 map 后，再显式启动主协程，避免时序歧义。
  void StartConnectionCoroutine(ConnectionState* state);

  // Drain 流程：先 drain wake fd，再 drain pending queue。
  bool DrainPendingConnections(std::string* error_msg);
  bool DrainCompletedResponses(std::string* error_msg);
  bool DrainWakeFd(std::string* error_msg);
  bool UpdateEpollInterest(int fd, ConnectionState& state,
                           std::string* error_msg);
  bool DispatchRequestToThreadPool(int fd, std::uint64_t connection_token,
                                   Connection::DispatchRequest request,
                                   std::string* error_msg);
  void RecordMethodCall(std::string method, bool success);

  // 停止时收敛所有连接，确保协程退出和 fd 回收。
  void CloseAllConnections();

  // 跨线程唤醒 worker epoll_wait。
  void Wakeup();

  // worker 线程主循环。
  void Run();

  bool CloseConnection(int fd, std::string_view reason);
  bool EnsureOwnerThread(std::string* error_msg) const;

  // ==========================================================================
  // 成员变量
  // ==========================================================================

  std::size_t worker_id_;                 // Worker 标识号
  const ServiceRegistry& registry_;       // 服务注册表引用
  rpc::common::ThreadPool* thread_pool_{nullptr};  // 可选的线程池指针

  rpc::common::UniqueFd epoll_fd_;        // epoll 实例文件描述符
  rpc::common::UniqueFd wake_fd_;         // eventfd，用于跨线程唤醒 epoll_wait
  std::shared_ptr<CompletedQueue> completed_queue_;  // 已完成响应队列

  // 连接状态映射表：fd -> ConnectionState
  std::unordered_map<int, std::unique_ptr<ConnectionState>> connections_;

  // acceptor -> worker 的跨线程移交队列
  std::mutex pending_mutex_;
  std::deque<PendingConnection> pending_connections_;

  // ==========================================================================
  // 原子状态变量
  // ==========================================================================

  std::atomic<bool> stop_requested_{false};          // 停止请求标志
  std::atomic<bool> accepting_new_connections_{false}; // 是否接受新连接
  std::atomic<std::size_t> connection_count_{0};     // 当前连接数
  std::atomic<std::size_t> total_accepted_count_{0}; // 累计接管连接数
  std::atomic<std::size_t> in_flight_request_count_{0}; // 处理中的请求数
  std::atomic<std::size_t> submitted_request_count_{0}; // 已提交请求数
  std::atomic<std::size_t> completed_request_count_{0}; // 已完成请求数
  std::atomic<std::uint64_t> next_connection_token_{1}; // 下一个连接令牌

  // ==========================================================================
  // 其他成员变量
  // ==========================================================================

  mutable std::mutex method_stats_mutex_;  // 方法统计互斥锁
  // 方法统计：method -> (总调用次数, 失败次数)
  std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> method_stats_;

  std::thread thread_;              // 工作线程
  std::thread::id owner_thread_id_{}; // owner 线程 ID
};

}  // namespace rpc::server
