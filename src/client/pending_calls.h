#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "client/rpc_types.h"

namespace rpc::client {

/// 待处理调用管理器
///
/// PendingCalls 用于异步 RPC 场景，管理 "请求ID -> 结果槽位" 的映射。
/// 它是 Promise/Future 模式的核心组件，支持：
/// - 发送请求时创建槽位（Add）
/// - 收到响应时填充结果（Complete）
/// - 等待者获取结果（Pop）
///
/// 使用场景：
/// ┌──────────┐                    ┌──────────┐
/// │ 发送线程 │                    │ 接收线程 │
/// └────┬─────┘                    └────┬─────┘
///      │ Add(request_id)               │
///      │ 发送请求...                    │
///      │                               │ 收到响应
///      │                               │ Complete(request_id, result)
///      │ Pop(request_id) → 等待...     │
///      │ Pop 返回 result               │
///      ↓                               ↓
///
/// 线程安全：所有方法都通过 mutex 保护，可在多线程环境下使用。
class PendingCalls {
 public:
  /// 添加一个待处理的请求槽位
  ///
  /// 在发送请求前调用，为该请求创建一个空的结果槽位。
  /// 后续响应到达时，通过 Complete 填充结果。
  ///
  /// @param request_id 请求的唯一标识符
  /// @return 成功返回 true，失败（空ID或重复ID）返回 false
  [[nodiscard]] bool Add(std::string request_id);

  /// 完成一个请求，填充结果
  ///
  /// 当收到服务端响应时调用，将结果写入对应的槽位。
  /// 正在等待该请求的 Pop 调用将能够获取结果。
  ///
  /// @param request_id 请求的唯一标识符
  /// @param result RPC 调用的结果
  /// @return 成功返回 true，找不到对应槽位返回 false
  [[nodiscard]] bool Complete(std::string_view request_id,
                              RpcCallResult result);

  /// 弹出并返回已完成的请求结果
  ///
  /// 获取指定请求的结果。如果请求尚未完成，返回 nullopt。
  /// 获取成功后会移除该槽位。
  ///
  /// @param request_id 请求的唯一标识符
  /// @return 如果请求已完成，返回结果；否则返回 nullopt
  [[nodiscard]] std::optional<RpcCallResult> Pop(std::string_view request_id);

  /// 将所有待处理请求标记为失败
  ///
  /// 当连接断开或发生严重错误时调用，
  /// 将所有等待中的请求标记为失败并填充统一的结果。
  ///
  /// @param result_template 失败时返回的结果模板
  void FailAll(const RpcCallResult& result_template);

  /// 获取当前待处理的请求数量
  /// @return 槽位数量
  [[nodiscard]] std::size_t Size() const;

 private:
  /// 结果槽位结构
  /// 每个请求对应一个槽位，用于存储完成状态和结果
  struct Slot {
    bool done{false};           ///< 是否已完成（收到响应）
    RpcCallResult result;       ///< RPC 调用结果
  };

  mutable std::mutex mutex_;                      ///< 保护 slots_ 的互斥锁
  std::unordered_map<std::string, Slot> slots_;   ///< request_id -> Slot 映射
};

}  // namespace rpc::client
