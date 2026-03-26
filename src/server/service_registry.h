/**
 * @file service_registry.h
 * @brief RPC 服务注册表模块
 * 
 * 本文件定义了 ServiceRegistry 类，实现了 RPC 服务方法的注册与查找功能。
 * ServiceRegistry 是 RPC 框架的核心组件，负责管理所有可调用的 RPC 方法。
 * 
 * ## 功能概述
 * 
 * - **方法注册**：将服务名+方法名映射到处理函数
 * - **方法查找**：根据请求查找对应的处理函数
 * - **线程安全**：支持并发注册和查找
 * 
 * ## 架构设计
 * 
 * ```
 *                    +------------------------+
 *                    |    ServiceRegistry     |
 *                    |------------------------|
 *  Register() -----> |  handlers_ (map)       |
 *                    |    "Service.Method" -> Handler
 *  Find() ---------> |                        |
 *                    +------------------------+
 * ```
 * 
 * ## 使用示例
 * 
 * @code
 *   ServiceRegistry registry;
 *   
 *   // 注册方法
 *   registry.Register("Calculator", "Add", [](std::string_view req) {
 *     AddRequest request;
 *     request.ParseFromString(req);
 *     AddResponse response;
 *     response.set_result(request.a() + request.b());
 *     return response.SerializeAsString();
 *   });
 *   
 *   // 查找方法
 *   auto handler = registry.Find("Calculator", "Add");
 *   if (handler) {
 *     std::string result = handler->get()(request_payload);
 *   }
 * @endcode
 * 
 * @see Connection 使用 ServiceRegistry 处理请求
 * @author RPC Framework Team
 * @date 2024
 */

#pragma once

#include <functional>       // std::function
#include <mutex>            // std::mutex, std::scoped_lock
#include <optional>         // std::optional
#include <stdexcept>        // std::runtime_error
#include <string>           // std::string
#include <string_view>      // std::string_view
#include <unordered_map>    // std::unordered_map

#include "common/rpc_error.h"  // RPC 错误定义

namespace rpc::server {

// ============================================================================
// 类型别名定义
// ============================================================================

/**
 * @brief RPC 状态码类型别名
 * 
 * 复用 common 命名空间的错误码体系，保持框架内部一致性。
 * 避免在 server 命名空间中定义重复的错误码。
 */
using RpcStatusCode = rpc::common::ErrorCode;

/**
 * @brief RPC 异常类型别名
 * 
 * 复用 common 命名空间的异常类型，
 * 允许 handler 抛出统一的异常格式。
 */
using RpcError = rpc::common::RpcException;

/**
 * @brief RPC 方法处理函数类型
 * 
 * 定义了 RPC 方法处理函数的签名：
 * - 输入：请求负载（二进制数据，通常是序列化的 Protobuf 消息）
 * - 输出：响应负载（二进制数据，序列化的 Protobuf 响应消息）
 * 
 * ## 使用约定
 * 
 * - 处理函数负责反序列化请求和序列化响应
 * - 处理函数可以抛出 RpcError 异常表示业务错误
 * - 处理函数不应抛出其他异常（会被框架捕获并转换为内部错误）
 * 
 * ## 示例
 * 
 * @code
 *   Handler add_handler = [](std::string_view request_payload) {
 *     AddRequest req;
 *     req.ParseFromString(std::string(request_payload));
 *     
 *     AddResponse resp;
 *     resp.set_result(req.a() + req.b());
 *     
 *     return resp.SerializeAsString();
 *   };
 * @endcode
 */
using Handler = std::function<std::string(std::string_view request_payload)>;

/**
 * @class ServiceRegistry
 * @brief RPC 服务方法注册表
 * 
 * ServiceRegistry 管理所有注册的 RPC 方法，提供服务名到处理函数的映射。
 * 
 * ## 核心功能
 * 
 * 1. **方法注册** (Register)
 *    - 将 "服务名.方法名" 映射到处理函数
 *    - 不允许重复注册同一方法
 * 
 * 2. **方法查找** (Find)
 *    - 根据服务名和方法名查找处理函数
 *    - 返回可选的引用包装器
 * 
 * ## 线程安全
 * 
 * ServiceRegistry 的所有公共方法都是线程安全的：
 * - 使用 std::mutex 保护内部数据结构
 * - 支持并发注册和查找
 * 
 * ## 键格式
 * 
 * 内部使用 "服务名.方法名" 作为键，例如：
 * - "Calculator.Add"
 * - "UserService.GetUser"
 * - "OrderService.CreateOrder"
 * 
 * @note 服务名和方法名不能为空
 * @note 处理函数不能为空（无效的 std::function）
 */
class ServiceRegistry {
 public:
  /**
   * @brief 注册 RPC 方法
   * 
   * 将指定服务名和方法名的处理函数注册到注册表。
   * 
   * @param service_name 服务名（如 "Calculator"）
   * @param method_name 方法名（如 "Add"）
   * @param handler 处理函数，签名为 `std::string(std::string_view)`
   * 
   * @return true 注册成功
   * @return false 注册失败（参数为空或方法已注册）
   * 
   * ## 注册规则
   * 
   * - 服务名和方法名都不能为空
   * - 处理函数必须有效（非空的 std::function）
   * - 同一 "服务名.方法名" 只能注册一次
   * 
   * ## 示例
   * 
   * @code
   *   ServiceRegistry registry;
   *   
   *   // 注册成功
   *   bool ok = registry.Register("Calculator", "Add", my_handler);
   *   
   *   // 重复注册失败
   *   bool fail = registry.Register("Calculator", "Add", another_handler);
   *   
   *   // 空参数失败
   *   bool fail2 = registry.Register("", "Add", my_handler);
   * @endcode
   * 
   * @note 此方法是线程安全的
   */
  [[nodiscard]] bool Register(std::string_view service_name,
                              std::string_view method_name, Handler handler);

  /**
   * @brief 查找 RPC 方法处理函数
   * 
   * 根据服务名和方法名查找注册的处理函数。
   * 
   * @param service_name 服务名
   * @param method_name 方法名
   * 
   * @return 找到则返回处理函数的常量引用包装器
   * @return 未找到则返回 std::nullopt
   * 
   * ## 返回值说明
   * 
   * 返回 `std::optional<std::reference_wrapper<const Handler>>`：
   * - `std::optional` 表示可能找不到方法
   * - `std::reference_wrapper` 避免复制 std::function
   * - `const Handler` 确保处理函数不会被修改
   * 
   * ## 使用示例
   * 
   * @code
   *   auto handler_opt = registry.Find("Calculator", "Add");
   *   if (handler_opt.has_value()) {
   *     // 获取实际的 Handler 引用
   *     const Handler& handler = handler_opt->get();
   *     // 或者直接调用
   *     std::string response = handler_opt->get()(request_payload);
   *   } else {
   *     // 方法不存在
   *   }
   * @endcode
   * 
   * @note 此方法是线程安全的
   */
  [[nodiscard]] std::optional<std::reference_wrapper<const Handler>> Find(
      std::string_view service_name, std::string_view method_name) const;

 private:
  /**
   * @brief 构建内部存储键
   * 
   * 将服务名和方法名组合为内部使用的键。
   * 格式："服务名.方法名"
   * 
   * @param service_name 服务名
   * @param method_name 方法名
   * 
   * @return 组合后的键字符串
   * 
   * @note 此方法假设输入参数有效，不做空值检查
   * 
   * ## 实现细节
   * 
   * - 预分配足够容量避免多次内存分配
   * - 使用 '.' 作为分隔符
   * 
   * @code
   *   BuildKey("Calculator", "Add")  // 返回 "Calculator.Add"
   * @endcode
   */
  [[nodiscard]] static std::string BuildKey(std::string_view service_name,
                                            std::string_view method_name);

  // ===========================================================================
  // 成员变量
  // ===========================================================================

  /**
   * @brief 互斥锁，保护 handlers_ 的并发访问
   * 
   * 使用 mutable 允许在 const 方法（如 Find）中加锁。
   */
  mutable std::mutex mutex_;

  /**
   * @brief 方法处理函数映射表
   * 
   * 键格式："服务名.方法名"
   * 值：对应的处理函数
   * 
   * 使用 std::unordered_map 提供 O(1) 平均查找时间。
   */
  std::unordered_map<std::string, Handler> handlers_;
};

}  // namespace rpc::server