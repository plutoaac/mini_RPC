/**
 * @file service_registry.cpp
 * @brief RPC 服务注册表模块实现
 * 
 * 本文件实现了 ServiceRegistry 类的所有方法，包括：
 * - 方法键的构建
 * - RPC 方法的注册
 * - RPC 方法的查找
 * 
 * 实现要点：
 * - 使用互斥锁保证线程安全
 * - 使用 unordered_map 提供高效的查找性能
 * - 返回引用包装器避免不必要的复制
 * 
 * @see service_registry.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/service_registry.h"

#include <utility>  // std::move

namespace rpc::server {

// ============================================================================
// 辅助方法实现
// ============================================================================

/**
 * @brief 构建内部存储键实现
 * 
 * 将服务名和方法名组合为单一字符串键。
 * 
 * 实现细节：
 * 1. 预计算所需容量并一次性分配
 * 2. 按顺序追加服务名、分隔符、方法名
 * 
 * 性能优化：
 * - reserve() 预分配避免多次扩容
 * - append(string_view) 避免创建临时 string 对象
 * 
 * @param service_name 服务名（如 "Calculator"）
 * @param method_name 方法名（如 "Add"）
 * @return 组合键（如 "Calculator.Add"）
 */
std::string ServiceRegistry::BuildKey(std::string_view service_name,
                                      std::string_view method_name) {
  // 预分配容量：服务名长度 + 方法名长度 + 1（分隔符）
  // 这避免了字符串在追加过程中的多次重新分配
  std::string key;
  key.reserve(service_name.size() + method_name.size() + 1);
  
  // 按顺序追加各部分
  key.append(service_name);  // 追加服务名
  key.push_back('.');        // 追加分隔符
  key.append(method_name);   // 追加方法名
  
  return key;
}

// ============================================================================
// 方法注册实现
// ============================================================================

/**
 * @brief 注册 RPC 方法实现
 * 
 * 将处理函数注册到内部映射表中。
 * 
 * 处理流程：
 * 1. 验证参数有效性（非空服务名、方法名、处理函数）
 * 2. 构建存储键
 * 3. 检查是否已存在相同键
 * 4. 插入新的键值对
 * 
 * 线程安全：
 * - 使用 std::scoped_lock 加锁
 * - scoped_lock 在作用域结束时自动释放锁
 * 
 * @param service_name 服务名
 * @param method_name 方法名
 * @param handler 处理函数
 * @return true 注册成功
 * @return false 注册失败（参数无效或键已存在）
 */
bool ServiceRegistry::Register(std::string_view service_name,
                               std::string_view method_name, Handler handler) {
  // ===== 参数验证 =====
  // 检查服务名、方法名是否为空
  // 检查 handler 是否有效（operator bool 返回 true 表示可调用）
  if (service_name.empty() || method_name.empty() || !handler) {
    return false;
  }

  // ===== 加锁保护 =====
  // 使用 scoped_lock 加锁，保证并发安全
  // 注册表可能在并发场景下访问，统一加锁保护
  std::scoped_lock lock(mutex_);
  
  // 构建存储键
  const std::string key = BuildKey(service_name, method_name);
  
  // ===== 检查重复 =====
  // 使用 contains() 检查键是否已存在（C++20）
  // 如果已存在，拒绝重复注册
  if (handlers_.contains(key)) {
    return false;
  }
  
  // ===== 插入映射 =====
  // 使用 emplace 插入新键值对
  // std::move(handler) 移动 handler 避免复制
  // emplace 返回 pair<iterator, bool>，second 表示是否插入成功
  return handlers_.emplace(key, std::move(handler)).second;
}

// ============================================================================
// 方法查找实现
// ============================================================================

/**
 * @brief 查找 RPC 方法处理函数实现
 * 
 * 从内部映射表中查找指定方法的处理函数。
 * 
 * 处理流程：
 * 1. 加锁保护并发访问
 * 2. 构建查找键
 * 3. 在映射表中查找
 * 4. 返回结果（引用包装器或 nullopt）
 * 
 * 返回值设计：
 * - 使用 std::optional 表示可能找不到
 * - 使用 std::reference_wrapper 避免复制 std::function
 * - 返回 const 引用确保处理函数不被修改
 * 
 * 性能考虑：
 * - unordered_map 的 find 操作平均时间复杂度为 O(1)
 * - 加锁粒度最小化，只保护实际的查找操作
 * 
 * @param service_name 服务名
 * @param method_name 方法名
 * @return 找到则返回处理函数的常量引用包装器
 * @return 未找到则返回 std::nullopt
 */
std::optional<std::reference_wrapper<const Handler>> ServiceRegistry::Find(
    std::string_view service_name, std::string_view method_name) const {
  // ===== 加锁保护 =====
  // 查找与注册共享同一把锁，保证线程安全
  // 使用 scoped_lock 确保异常安全
  std::scoped_lock lock(mutex_);
  
  // 构建查找键
  const std::string key = BuildKey(service_name, method_name);
  
  // 在映射表中查找
  // find() 返回迭代器，end() 表示未找到
  const auto it = handlers_.find(key);
  
  // 检查是否找到
  if (it == handlers_.end()) {
    // 未找到，返回空的 optional
    return std::nullopt;
  }
  
  // 找到，返回处理函数的常量引用包装器
  // std::cref 创建对 Handler 的 const 引用包装器
  // it->second 是 Handler 对象
  return std::cref(it->second);
}

}  // namespace rpc::server