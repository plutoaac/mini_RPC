#pragma once

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "client/rpc_types.h"

namespace rpc::client {

/// RPC 调用请求管理器
///
/// PendingCalls 用于管理进行中的 RPC 调用请求，维护 request_id
/// 到结果槽位的映射。 它是 RPC 客户端实现"调用线程等待 +
/// 响应线程分发"并发模型的核心组件。
///
/// 设计模式：
/// - 每个 RPC 调用创建一个 Slot，包含完成状态、结果和条件变量
/// - 调用线程通过 WaitAndPop 阻塞等待响应
/// - Dispatcher 线程通过 Complete 设置结果并唤醒等待线程
///
/// 线程安全：
/// - 所有公共方法都是线程安全的
/// - 使用互斥锁保护内部数据结构
/// - 使用条件变量实现高效的等待/唤醒机制
///
/// 使用示例：
/// @code
/// PendingCalls pending;
/// pending.Add("req-123");                    // 添加请求
/// // 线程1: 等待响应
/// auto result = pending.WaitAndPop("req-123", 2000ms);
/// // 线程2: 设置响应
/// pending.Complete("req-123", RpcCallResult{...});
/// @endcode
class PendingCalls {
 public:
  /// 添加一个新的请求槽位
  ///
  /// 创建一个未完成状态的 Slot 用于后续接收响应。
  /// 如果 request_id 已存在或为空，则添加失败。
  ///
  /// @param request_id 请求唯一标识符
  /// @return 添加成功返回 true，失败（重复或空 ID）返回 false
  [[nodiscard]] bool Add(std::string request_id);

  /// 绑定异步等待者
  ///
  /// 为已存在的请求槽位绑定 promise 与超时信息。
  /// 若响应已先到达，会立即完成 promise 并移除槽位。
  [[nodiscard]] bool BindAsync(std::string_view request_id,
                               std::promise<RpcCallResult> promise,
                               std::chrono::steady_clock::time_point deadline);

  /// 完成一个请求并设置结果
  ///
  /// 由 Dispatcher 线程调用，将响应结果写入对应的槽位，
  /// 并唤醒正在等待该请求的线程。
  ///
  /// @param request_id 请求唯一标识符
  /// @param result RPC 调用结果
  /// @return 找到并完成成功返回 true，请求不存在返回 false
  [[nodiscard]] bool Complete(std::string_view request_id,
                              RpcCallResult result);

  /// 等待请求完成并取出结果（阻塞）
  ///
  /// 阻塞当前线程直到收到响应或超时。
  /// 成功获取结果后会自动移除该槽位。
  ///
  /// @param request_id 请求唯一标识符
  /// @param timeout 超时时间
  /// @return 成功返回结果，超时或请求不存在返回 nullopt
  [[nodiscard]] std::optional<RpcCallResult> WaitAndPop(
      std::string_view request_id, std::chrono::milliseconds timeout);

  /// 非阻塞获取已完成结果
  ///
  /// 尝试立即获取已完成的结果，不会阻塞。
  /// 如果请求未完成或不存在则返回 nullopt。
  /// 成功获取后会移除该槽位。
  ///
  /// @param request_id 请求唯一标识符
  /// @return 已完成返回结果，未完成或不存在返回 nullopt
  [[nodiscard]] std::optional<RpcCallResult> TryPop(
      std::string_view request_id);

  /// 删除某个请求槽位
  ///
  /// 用于调用超时后的清理，防止槽位泄漏。
  /// 注意：如果有线程正在等待该请求，删除后等待会返回 nullopt。
  ///
  /// @param request_id 请求唯一标识符
  void Remove(std::string_view request_id);

  /// 将所有待处理请求标记为失败
  ///
  /// 当连接断开或发生严重错误时调用。
  /// 所有等待中的请求都会收到相同的错误结果，并被唤醒。
  ///
  /// @param result_template 失败结果模板，将应用于所有请求
  void FailAll(const RpcCallResult& result_template);

  /// 使已到 deadline 的异步请求失败
  ///
  /// 由 dispatcher 在读超时分支中调用，避免每请求一个等待线程。
  void FailTimedOut(std::chrono::steady_clock::time_point now,
                    const RpcCallResult& timeout_result);

  /// 获取当前待处理请求数量
  ///
  /// @return 当前槽位数量
  [[nodiscard]] std::size_t Size() const;

 private:
  /// 请求槽位结构
  ///
  /// 存储单个 RPC 调用的状态和结果。
  /// 每个槽位关联一个条件变量用于等待/唤醒机制。
  struct Slot {
    /// 是否已完成（收到响应）
    bool done{false};
    /// RPC 调用结果
    RpcCallResult result;
    /// 条件变量，用于阻塞等待响应
    std::condition_variable cv;
    /// 异步 promise（存在时表示该请求由 CallAsync 等待）
    std::optional<std::promise<RpcCallResult>> async_promise;
    /// 是否已绑定异步等待者
    bool async_bound{false};
    /// 异步请求的 deadline（仅 async_bound=true 时有效）
    std::chrono::steady_clock::time_point deadline{};
  };

  /// 互斥锁，保护 slots_ 的并发访问
  mutable std::mutex mutex_;
  /// 请求 ID -> 槽位的映射表
  std::unordered_map<std::string, Slot> slots_;
};

}  // namespace rpc::client