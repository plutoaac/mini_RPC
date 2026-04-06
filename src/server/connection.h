/**
 * @file connection.h
 * @brief RPC 服务端连接处理模块
 *
 * 本文件定义了 Connection 类，负责处理单个客户端连接的所有 I/O 操作。
 * Connection 是 RPC 服务端处理请求的核心组件，实现了完整的请求-响应流程。
 *
 * ## 架构概述
 *
 * ```
 *                    +------------------------+
 *                    |      Connection        |
 *                    |------------------------|
 *  Client <-------> |  fd_ (socket)          |
 *                    |  read_buffer_          |
 *                    |  write_buffer_         |
 *                    |  registry_ (方法查找)  |
 *                    +------------------------+
 *                              |
 *                              v
 *                    +------------------------+
 *                    |    ServiceRegistry     |
 *                    |    (查找处理函数)       |
 *                    +------------------------+
 * ```
 *
 * ## 数据流
 *
 * ```
 *  客户端请求流：
 *  [Client] -> recv() -> read_buffer_ -> 解析帧 -> 反序列化 -> 处理函数
 *
 *  服务端响应流：
 *  [处理函数] -> 序列化 -> 编码帧 -> write_buffer_ -> send() -> [Client]
 * ```
 *
 * ## 帧协议
 *
 * 采用简单的长度前缀帧协议：
 * ```
 *  +----------------+------------------+
 *  | 4 bytes header |    N bytes body  |
 *  +----------------+------------------+
 *  | body length    |  RpcRequest/     |
 *  | (big-endian)   |  RpcResponse     |
 *  +----------------+------------------+
 * ```
 *
 * ## 主要功能
 *
 * 1. **数据读取**：从 socket 读取数据到读缓冲区
 * 2. **请求解析**：从缓冲区解析完整的 RPC 请求帧
 * 3. **请求处理**：调用 ServiceRegistry 查找并执行处理函数
 * 4. **响应发送**：将响应写入写缓冲区并发送
 *
 * ## 使用示例
 *
 * @code
 *   // Connection 通常由 RpcServer 创建和管理
 *   // 以下是独立的同步使用示例：
 *
 *   ServiceRegistry registry;
 *   registry.Register("Calculator", "Add", AddHandler);
 *
 *   Connection conn(std::move(client_fd), registry);
 *   bool ok = conn.Serve();  // 阻塞服务直到连接关闭
 * @endcode
 *
 * @see RpcServer 管理 Connection 的生命周期
 * @see ServiceRegistry 提供方法查找功能
 * @author RPC Framework Team
 * @date 2024
 */

#pragma once

#include <chrono>     // steady_clock, milliseconds
#include <coroutine>  // std::coroutine_handle
#include <cstddef>    // std::size_t
#include <cstdint>
#include <deque>  // std::deque
#include <functional>
#include <optional>  // std::optional
#include <string>    // std::string
#include <string_view>
#include <thread>

#include "common/unique_fd.h"  // RAII 文件描述符封装
#include "coroutine/task.h"

// 前向声明 Protobuf 消息类型，避免在头文件中包含 proto 生成的头文件
namespace rpc {
class RpcRequest;   // RPC 请求消息
class RpcResponse;  // RPC 响应消息
}  // namespace rpc

namespace rpc::server {

// 前向声明
class ServiceRegistry;

/**
 * @class Connection
 * @brief 单个客户端连接的处理对象
 *
 * Connection 类封装了处理单个 RPC 客户端连接所需的所有状态和方法。
 *
 * ## 核心职责
 *
 * 1. **Socket I/O 管理**
 *    - 从 socket 读取数据
 *    - 向 socket 写入数据
 *    - 处理非阻塞 I/O 的边界情况
 *
 * 2. **协议处理**
 *    - 帧编码/解码（长度前缀协议）
 *    - 消息序列化/反序列化
 *
 * 3. **请求分发**
 *    - 根据 ServiceRegistry 查找处理函数
 *    - 调用处理函数并处理异常
 *
 * ## 缓冲区管理
 *
 * - `read_buffer_`：存储从 socket 接收的原始数据
 * - `write_buffer_`：存储待发送的响应数据
 *
 * 两个缓冲区都采用线性结构，处理完的数据从头部移除。
 *
 * ## 状态管理
 *
 * - `should_close_`：标记连接是否应该关闭
 * - `MarkClosing()`：将连接标记为即将关闭
 * - `ShouldClose()`：检查是否应该关闭连接
 *
 * ## 线程安全
 *
 * Connection 不是线程安全的，应该在单个线程中处理。
 * 在 RpcServer 中，每个 Connection 只在主事件循环中访问。
 */
class Connection {
 public:
  struct DispatchRequest {
    std::uint64_t sequence{0};
    std::string request_id;
    std::string service_name;
    std::string method_name;
    std::string payload;
  };

  using RequestDispatchFn =
      std::function<bool(DispatchRequest request, std::string* error_msg)>;

  /**
   * @brief 连接状态枚举
   *
   * 状态转换图：
   * ```
   *   kOpen ──┬──> kReading ──> kOpen (读取完成)
   *           │       │
   *           │       └──> kError (读取错误)
   *           │
   *           ├──> kWriting ──> kOpen (发送完成)
   *           │       │
   *           │       └──> kError (发送错误)
   *           │
   *           ├──> kClosing ──> kClosed
   *           │       │
   *           │       └──> kError (关闭过程中出错)
   *           │
   *           └──> kError
   * ```
   */
  enum class State {
    kOpen,     ///< 连接空闲，可以接受新请求
    kReading,  ///< 连接正在等待读取数据（协程挂起在 WaitReadableCo）
    kWriting,  ///< 连接正在等待发送数据（协程挂起在 WaitWritableCo）
    kClosing,  ///< 连接正在关闭中（收到 EPOLLHUP/EPOLLRDHUP 或调用
               ///< MarkClosing）
    kClosed,   ///< 连接已关闭（资源已释放）
    kError,    ///< 连接发生错误（读/写超时、socket 错误等）
  };

  struct Options {
    std::chrono::milliseconds read_timeout{std::chrono::milliseconds(5000)};
    std::chrono::milliseconds write_timeout{std::chrono::milliseconds(5000)};
    std::size_t max_write_buffer_bytes{8 * 1024 * 1024};
  };

  /**
   * @brief 构造 Connection 对象
   *
   * @param fd 客户端 socket 的文件描述符（通过 RAII 封装）
   * @param registry 服务注册表的常量引用
   *
   * @note fd 将被移动到 Connection 内部，调用者不应再使用该 fd
   * @note registry 必须在 Connection 整个生命周期内保持有效
   *
   * ## 初始化操作
   *
   * - 移动接管文件描述符
   * - 保存服务注册表引用
   * - 预分配缓冲区容量（减少后续重新分配）
   */
  Connection(rpc::common::UniqueFd fd, const ServiceRegistry& registry);
  Connection(rpc::common::UniqueFd fd, const ServiceRegistry& registry,
             Options options);

  // =========================================================================
  // 事件处理方法
  // =========================================================================

  /**
   * @brief 处理可读事件
   *
   * 当 socket 变为可读时调用此方法。
   *
   * ## 处理流程
   *
   * 1. 从 socket 读取所有可用数据到 read_buffer_
   * 2. 尝试从缓冲区解析完整的请求帧
   * 3. 对于每个完整的请求：
   *    a. 反序列化为 RpcRequest
   *    b. 查找并调用处理函数
   *    c. 序列化响应并加入写缓冲区
   * 4. 如果对端关闭连接，设置 should_close_ 标志
   *
   * @param error_msg 如果失败，输出错误信息
   * @return true 处理成功（可能已读取部分数据或对端关闭）
   * @return false 发生不可恢复的错误
   *
   * ## 错误处理
   *
   * - 返回 false 表示连接应该关闭
   * - 对端正常关闭返回 true（should_close_ 为 true）
   * - 协议解析错误会返回错误响应，但不会导致 false 返回
   *
   * @note 此方法是非阻塞的，不会等待数据到达
   */
  [[nodiscard]] bool OnReadable(std::string* error_msg);

  /**
   * @brief 处理可写事件
   *
   * 当 socket 变为可写时调用此方法。
   *
   * ## 处理流程
   *
   * 1. 尽可能多地发送 write_buffer_ 中的数据
   * 2. 从缓冲区移除已发送的数据
   *
   * @param error_msg 如果失败，输出错误信息
   * @return true 发送成功（可能还有数据未发送完）
   * @return false 发生发送错误（连接应该关闭）
   *
   * @note 此方法是非阻塞的，可能只发送部分数据
   */
  [[nodiscard]] bool OnWritable(std::string* error_msg);

  // =========================================================================
  // 状态查询方法
  // =========================================================================

  /**
   * @brief 检查是否有待发送的数据
   *
   * @return true 写缓冲区非空，有待发送数据
   * @return false 写缓冲区为空
   *
   * 用于决定是否需要注册 EPOLLOUT 事件。
   */
  [[nodiscard]] bool HasPendingWrite() const noexcept;

  /**
   * @brief 检查连接是否应该关闭
   *
   * @return true 连接应该关闭
   * @return false 连接应该继续
   *
   * ## 触发关闭的条件
   *
   * - 对端关闭连接（recv 返回 0）
   * - 收到 EPOLLHUP/EPOLLRDHUP 事件
   * - 调用了 MarkClosing()
   */
  [[nodiscard]] bool ShouldClose() const noexcept;
  [[nodiscard]] State GetState() const noexcept;
  [[nodiscard]] const char* StateName() const noexcept;
  [[nodiscard]] const std::string& LastError() const noexcept;
  [[nodiscard]] bool HasRequestDispatcher() const noexcept;

  // 绑定连接到某个 WorkerLoop。当前线程会被记录为 owner thread。
  void BindToWorkerLoop(std::size_t worker_id) noexcept;
  [[nodiscard]] bool IsBoundToWorkerLoop() const noexcept;
  [[nodiscard]] std::optional<std::size_t> OwnerWorkerId() const noexcept;
  [[nodiscard]] bool IsOnOwnerThread() const noexcept;

  /**
   * @brief 将连接标记为即将关闭
   *
   * 在当前处理完成后，连接将被关闭。
   * 通常在收到 EPOLLHUP/EPOLLRDHUP 事件时调用。
   */
  void MarkClosing() noexcept;
  void MarkClosed() noexcept;
  void Tick(std::chrono::steady_clock::time_point now) noexcept;

  // =========================================================================
  // 同步服务方法
  // =========================================================================

  /**
   * @brief 同步服务循环（阻塞模式）
   *
   * 在阻塞模式下服务客户端，直到连接关闭。
   *
   * ## 处理流程
   *
   * 1. 确保 socket 为非阻塞模式
   * 2. 循环：
   *    a. 调用 OnReadable() 读取并处理请求
   *    b. 如果有待发送数据，调用 OnWritable()
   *    c. 如果 should_close_ 为 true，退出循环
   *
   * @return true 正常结束（对端关闭）
   * @return false 处理失败
   *
   * ## 使用场景
   *
   * - 单线程同步服务器
   * - 测试和调试
   *
   * @note 此方法会阻塞当前线程
   */
  [[nodiscard]] bool Serve();

  // =========================================================================
  // 协程友好接口（连接级）
  // =========================================================================

  class ReadableAwaiter {
   public:
    explicit ReadableAwaiter(Connection* connection)
        : connection_(connection) {}

    [[nodiscard]] bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    void await_resume() noexcept;

   private:
    Connection* connection_;
  };

  class WritableAwaiter {
   public:
    explicit WritableAwaiter(Connection* connection)
        : connection_(connection) {}

    [[nodiscard]] bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> handle) noexcept;
    void await_resume() noexcept;

   private:
    Connection* connection_;
  };

  [[nodiscard]] ReadableAwaiter WaitReadableCo() noexcept;
  [[nodiscard]] WritableAwaiter WaitWritableCo() noexcept;

  [[nodiscard]] rpc::coroutine::Task<bool> ReadRequestCo(
      std::string* error_msg);
  [[nodiscard]] rpc::coroutine::Task<bool> WriteResponseCo(
      std::string* error_msg);

  void NotifyReadable() noexcept;
  void NotifyWritable() noexcept;

  void SetRequestDispatcher(RequestDispatchFn dispatcher);

  [[nodiscard]] bool EnqueueResponse(const rpc::RpcResponse& response,
                                     std::string* error_msg);

 private:
  // =========================================================================
  // 内部类型定义
  // =========================================================================

  /**
   * @enum ReadResult
   * @brief socket 读取结果枚举
   *
   * 用于区分不同类型的读取结果。
   */
  enum class ReadResult {
    kWouldBlock,  ///< 非阻塞模式下没有数据可读
    kData,        ///< 成功读取了数据
    kPeerClosed,  ///< 对端关闭了连接
    kError,       ///< 发生错误
  };

  // =========================================================================
  // 内部方法
  // =========================================================================

  /**
   * @brief 从 socket 读取数据到读缓冲区
   *
   * 非阻塞地读取 socket 数据。
   *
   * @param error_msg 如果失败，输出错误信息
   * @return 读取结果枚举值
   */
  [[nodiscard]] ReadResult ReadFromSocket(std::string* error_msg);

  /**
   * @brief 尝试从读缓冲区解析并处理请求
   *
   * 循环解析缓冲区中的完整帧，并处理每个请求。
   *
   * @param error_msg 如果失败，输出错误信息
   * @return true 处理成功
   * @return false 发生不可恢复的错误
   */
  [[nodiscard]] bool TryParseRequests(std::string* error_msg);

  /**
   * @brief 处理单个 RPC 请求
   *
   * 查找处理函数并调用，生成响应。
   *
   * @param request RPC 请求消息
   * @param response 输出的 RPC 响应消息
   * @return true 处理成功
   * @return false 处理失败（通常是内部错误）
   *
   * @note 即使方法不存在或处理抛出异常，也会返回 true 并设置错误响应
   */
  [[nodiscard]] bool HandleOneRequest(const rpc::RpcRequest& request,
                                      rpc::RpcResponse* response) const;

  /**
   * @brief 将响应加入写缓冲区
   *
   * 序列化响应并编码为帧格式。
   *
   * @param response RPC 响应消息
   * @param error_msg 如果失败，输出错误信息
   * @return true 成功加入缓冲区
   * @return false 序列化或编码失败
   */
  [[nodiscard]] bool QueueResponse(const rpc::RpcResponse& response,
                                   std::string* error_msg);

  /**
   * @brief 发送写缓冲区中的数据
   *
   * 非阻塞地发送尽可能多的数据。
   *
   * @param error_msg 如果失败，输出错误信息
   * @return true 发送成功（可能还有数据未发送完）
   * @return false 发生发送错误
   */
  [[nodiscard]] bool FlushWrites(std::string* error_msg);

  // =========================================================================
  // 静态辅助方法
  // =========================================================================

  /**
   * @brief 编码帧（添加长度前缀）
   *
   * @param body 帧体数据
   * @param frame 输出的完整帧数据
   * @param error_msg 如果失败，输出错误信息
   * @return true 编码成功
   * @return false 参数无效
   */
  [[nodiscard]] static bool EncodeFrame(const std::string& body,
                                        std::string* frame,
                                        std::string* error_msg);

  /**
   * @brief 解码帧头（读取长度前缀）
   *
   * @param buffer 缓冲区数据
   * @param offset 在缓冲区中的偏移量
   * @param body_length 输出的帧体长度
   * @param error_msg 如果失败，输出错误信息
   * @return true 解码成功
   * @return false 帧头无效
   */
  [[nodiscard]] static bool DecodeFrameHeader(const std::string& buffer,
                                              std::size_t offset,
                                              std::size_t* body_length,
                                              std::string* error_msg);

  void ResumeWaiter(std::coroutine_handle<>& waiter) noexcept;
  void AssertOwnerThread() const noexcept;
  void EnterError(std::string message) noexcept;
  void ArmReadDeadline() noexcept;
  void ArmWriteDeadline() noexcept;
  void ClearReadDeadline() noexcept;
  void ClearWriteDeadline() noexcept;

  // =========================================================================
  // 成员变量
  // =========================================================================

  /**
   * @brief 客户端 socket 文件描述符
   *
   * 使用 RAII 封装，析构时自动关闭。
   */
  rpc::common::UniqueFd fd_;

  /**
   * @brief 服务注册表引用
   *
   * 用于查找 RPC 方法的处理函数。
   */
  const ServiceRegistry& registry_;

  Options options_;

  /**
   * @brief 读缓冲区
   *
   * 存储从 socket 接收但尚未处理的原始数据。
   */
  std::string read_buffer_;

  /**
   * @brief 写缓冲区
   *
   * 存储待发送的响应数据（已编码为帧格式）。
   * 采用分块队列结构，避免部分发送时的内存搬移。
   */
  struct PendingChunk {
    std::string data;
    std::size_t offset{0};  // 记录已发送的字节数
  };
  std::deque<PendingChunk> write_buffer_;
  std::size_t pending_write_bytes_{0};  // 记录待发送的总字节数

  std::string last_error_;
  State state_{State::kOpen};

  /**
   * @brief 关闭标志
   *
   * 为 true 时表示连接应该关闭。
   */
  bool should_close_{false};
  bool read_ready_{false};
  bool write_ready_{false};
  std::optional<std::chrono::steady_clock::time_point> read_deadline_;
  std::optional<std::chrono::steady_clock::time_point> write_deadline_;
  std::coroutine_handle<> read_waiter_{};
  std::coroutine_handle<> write_waiter_{};

  RequestDispatchFn request_dispatcher_;
  std::uint64_t next_request_sequence_{0};

  std::optional<std::size_t> owner_worker_id_;
  std::thread::id owner_thread_id_{};
};

}  // namespace rpc::server
