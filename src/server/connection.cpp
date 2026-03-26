/**
 * @file connection.cpp
 * @brief RPC 服务器连接处理模块实现
 *
 * 本文件实现了 Connection 类的所有方法，包括：
 * - 连接服务循环
 * - Socket 数据读写
 * - 协议帧解析与组装
 * - 请求处理与响应生成
 *
 * @see connection.h 头文件定义
 * @author RPC Framework Team
 * @date 2024
 */

#include "server/connection.h"

// 网络相关头文件
#include <arpa/inet.h>   // htonl, ntohl, inet_ntop
#include <sys/socket.h>  // socket, recv, send, accept

// 标准库头文件
#include <cerrno>       // errno 错误码
#include <cstring>      // strerror, memcpy
#include <exception>    // std::exception
#include <string>       // std::string
#include <string_view>  // std::string_view

// 项目内部头文件
#include "common/log.h"               // 日志输出
#include "common/rpc_error.h"         // RPC 错误码定义
#include "rpc.pb.h"                   // Protobuf 生成的消息定义
#include "server/service_registry.h"  // 服务注册表

namespace rpc::server {

// ============================================================================
// 匿名命名空间 - 内部常量定义
// ============================================================================

namespace {

/**
 * @brief 帧头字节数
 *
 * 协议使用 4 字节的大端序整数作为帧头，存储消息体长度。
 * 这允许单个消息最大达到 4GB（实际受限于 kMaxFrameSize）。
 */
constexpr std::size_t kFrameHeaderBytes = 4;

/**
 * @brief 最大帧大小限制（4MB）
 *
 * 防止恶意客户端发送超大消息导致内存耗尽。
 * 超过此限制的帧将被视为协议错误。
 */
constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024;

}  // namespace

// ============================================================================
// 构造函数实现
// ============================================================================

/**
 * @brief 构造函数实现
 *
 * 初始化连接实例，包括：
 * 1. 通过移动语义接管 socket 文件描述符
 * 2. 保存服务注册表引用
 * 3. 预分配读写缓冲区容量
 *
 * 缓冲区预分配策略：
 * - 初始容量 8KB，适合大多数小型请求
 * - 采用 std::string 的动态扩容机制处理大型请求
 * - 预分配减少首次分配开销和内存碎片
 */
Connection::Connection(rpc::common::UniqueFd fd,
                       const ServiceRegistry& registry)
    : fd_(std::move(fd)),    // 移动语义转移 fd 所有权
      registry_(registry) {  // 保存注册表引用
  // 预分配缓冲区容量，避免频繁扩容
  read_buffer_.reserve(8192);   // 8KB 接收缓冲区
  write_buffer_.reserve(8192);  // 8KB 发送缓冲区
}

// ============================================================================
// 服务循环实现
// ============================================================================

/**
 * @brief 服务循环实现
 *
 * 核心处理循环，持续执行直到连接关闭或发生错误。
 *
 * 处理流程：
 * ```
 * while (true) {
 *   1. 读取数据 -> 成功/对端关闭/错误
 *   2. 解析请求 -> 提取完整帧并处理
 *   3. 刷新发送 -> 将响应写回 socket
 * }
 * ```
 *
 * 循环终止条件：
 * - ReadFromSocket 返回 kPeerClosed：对端关闭连接，正常退出
 * - ReadFromSocket 返回 kError：读取错误，异常退出
 * - TryParseRequests 返回 false：协议解析严重错误
 * - FlushWrites 返回 false：发送错误
 */
bool Connection::Serve() {
  while (true) {
    // ===== 步骤 1：从 socket 读取数据 =====
    std::string read_error;
    const ReadResult read_result = ReadFromSocket(&read_error);

    if (read_result == ReadResult::kPeerClosed) {
      // 对端关闭连接，正常结束
      return true;
    }
    if (read_result == ReadResult::kError) {
      // 读取错误，记录日志并异常结束
      common::LogError("connection read failed: " + read_error);
      return false;
    }

    // ===== 步骤 2：解析并处理请求 =====
    std::string parse_error;
    if (!TryParseRequests(&parse_error)) {
      // 解析失败，记录日志并异常结束
      common::LogError("connection parse failed: " + parse_error);
      return false;
    }

    // ===== 步骤 3：刷新发送缓冲区 =====
    std::string write_error;
    if (!FlushWrites(&write_error)) {
      // 发送失败，记录日志并异常结束
      common::LogError("connection write failed: " + write_error);
      return false;
    }
  }
}

// ============================================================================
// 数据读取实现
// ============================================================================

/**
 * @brief 从 socket 读取数据实现
 *
 * 使用 recv() 系统调用从 socket 读取数据。
 *
 * 实现细节：
 * - 使用固定 4KB 的临时缓冲区进行读取
 * - 处理 EINTR 信号中断（自动重试）
 * - 返回值区分三种状态：数据、对端关闭、错误
 *
 * @note recv() 在阻塞 socket 上会等待数据到达
 * @note 对于非阻塞 socket，需要额外处理 EAGAIN/EWOULDBLOCK
 */
Connection::ReadResult Connection::ReadFromSocket(std::string* error_msg) {
  // 临时读取缓冲区，每次最多读取 4KB
  char chunk[4096];

  while (true) {
    // 调用 recv 读取数据
    // 参数：socket fd、缓冲区、缓冲区大小、标志位（0 = 阻塞读取）
    const ssize_t rc =
        ::recv(fd_.Get(), chunk, static_cast<std::size_t>(sizeof(chunk)), 0);

    if (rc > 0) {
      // 成功读取 rc 字节数据，追加到接收缓冲区
      read_buffer_.append(chunk, static_cast<std::size_t>(rc));
      return ReadResult::kData;
    }

    if (rc == 0) {
      // recv 返回 0 表示对端已关闭连接（收到 FIN）
      if (error_msg != nullptr) {
        *error_msg = "peer closed connection";
      }
      return ReadResult::kPeerClosed;
    }

    // rc < 0 表示发生错误
    if (errno == EINTR) {
      // 被信号中断，重试读取
      continue;
    }

    // 其他错误，记录错误信息
    if (error_msg != nullptr) {
      *error_msg = std::string("recv failed: ") + std::strerror(errno);
    }
    return ReadResult::kError;
  }
}

// ============================================================================
// 请求解析实现
// ============================================================================

/**
 * @brief 解析请求实现
 *
 * 从 read_buffer_ 中提取并处理所有完整的请求帧。
 *
 * 协议格式：
 * ```
 * +----------------+------------------+
 * | 4 bytes header | N bytes body     |
 * +----------------+------------------+
 * ```
 *
 * 解析流程：
 * 1. 检查是否有足够的字节读取帧头（至少 4 字节）
 * 2. 解析帧头获取消息体长度
 * 3. 检查是否已接收完整的消息体
 * 4. 反序列化并处理请求
 * 5. 更新偏移量，继续处理下一个帧
 *
 * 缓冲区管理：
 * - 解析完成后，移除已处理的数据（erase from front）
 * - 保留未完整接收的数据，等待更多数据到达
 */
bool Connection::TryParseRequests(std::string* error_msg) {
  std::size_t offset = 0;  // 当前处理位置

  // 循环处理缓冲区中的所有完整帧
  while (read_buffer_.size() - offset >= kFrameHeaderBytes) {
    // 尝试解码帧头，获取消息体长度
    std::size_t body_length = 0;
    if (!DecodeFrameHeader(read_buffer_, offset, &body_length, error_msg)) {
      return false;  // 帧头解析失败（非法帧长度）
    }

    // 检查是否已接收完整的消息体
    if (read_buffer_.size() - offset < kFrameHeaderBytes + body_length) {
      // 消息体不完整，等待更多数据
      break;
    }

    // 提取消息体视图（零拷贝）
    std::string_view body_view(read_buffer_.data() + offset + kFrameHeaderBytes,
                               body_length);

    // 解析 RpcRequest 并处理
    rpc::RpcRequest request;
    rpc::RpcResponse response;

    if (!request.ParseFromArray(body_view.data(),
                                static_cast<int>(body_view.size()))) {
      // ===== 解析失败：生成错误响应 =====
      // Protobuf 解析失败，请求格式非法
      response.set_request_id("");  // 无法获取 request_id
      response.set_error_code(rpc::PARSE_ERROR);
      response.set_error_msg("failed to parse RpcRequest");
      if (!QueueResponse(response, error_msg)) {
        return false;  // 无法发送响应，严重错误
      }
    } else {
      // ===== 解析成功：处理请求 =====
      if (!HandleOneRequest(request, &response)) {
        if (error_msg != nullptr) {
          *error_msg = "failed to handle request";
        }
        return false;
      }
      if (!QueueResponse(response, error_msg)) {
        return false;  // 无法发送响应，严重错误
      }
    }

    // 移动到下一个帧
    offset += kFrameHeaderBytes + body_length;
  }

  // 移除已处理的数据，保留未完整接收的数据
  if (offset > 0) {
    read_buffer_.erase(0, offset);
  }

  return true;
}

// ============================================================================
// 请求处理实现
// ============================================================================

/**
 * @brief 处理单个请求实现
 *
 * 执行 RPC 方法调用并生成响应。
 *
 * 处理流程：
 * 1. 初始化响应消息（设置 request_id，清空其他字段）
 * 2. 在服务注册表中查找处理方法
 * 3. 调用处理方法或生成错误响应
 * 4. 捕获并处理所有异常
 *
 * 异常处理策略：
 * - RpcError：转换为对应的错误码
 * - std::exception：转换为内部错误
 * - 未知异常：转换为内部错误
 *
 * @note 即使发生异常，也会返回 true 并生成错误响应
 * @note 只有 response 指针为空时才返回 false
 */
bool Connection::HandleOneRequest(const rpc::RpcRequest& request,
                                  rpc::RpcResponse* response) const {
  // 参数检查
  if (response == nullptr) {
    return false;
  }

  // 初始化响应消息
  // 设置 request_id 用于客户端匹配请求与响应
  response->set_request_id(request.request_id());
  response->set_error_code(rpc::OK);  // 默认成功
  response->clear_error_msg();        // 清空错误消息
  response->clear_payload();          // 清空响应体

  // 在服务注册表中查找处理方法
  const auto handler =
      registry_.Find(request.service_name(), request.method_name());

  if (!handler.has_value()) {
    // 方法未找到：生成错误响应
    response->set_error_code(rpc::METHOD_NOT_FOUND);
    response->set_error_msg("method not found: " + request.service_name() +
                            "." + request.method_name());
    return true;  // 返回 true，错误已正确处理
  }

  // 调用处理方法
  try {
    // 执行处理函数，返回序列化后的响应体
    // handler->get() 获取 std::reference_wrapper 包装的实际 Handler
    response->set_payload(handler->get()(request.payload()));
    return true;
  } catch (const RpcError& ex) {
    // 捕获 RPC 业务异常，转换为错误响应
    response->set_error_code(common::ToProtoErrorCode(ex.code()));
    response->set_error_msg(ex.what());
    return true;
  } catch (const std::exception& ex) {
    // 捕获标准异常，转换为内部错误
    response->set_error_code(common::ToProtoErrorCode(
        common::make_error_code(common::ErrorCode::kInternalError)));
    response->set_error_msg(std::string("handler exception: ") + ex.what());
    return true;
  } catch (...) {
    // 捕获未知异常，转换为内部错误
    response->set_error_code(common::ToProtoErrorCode(
        common::make_error_code(common::ErrorCode::kInternalError)));
    response->set_error_msg("handler threw unknown exception");
    return true;
  }
}

// ============================================================================
// 响应队列实现
// ============================================================================

/**
 * @brief 响应入队实现
 *
 * 将 RpcResponse 序列化并添加帧头，追加到发送缓冲区。
 *
 * 处理流程：
 * 1. 获取序列化后的消息体大小
 * 2. 检查帧大小合法性
 * 3. 序列化 RpcResponse 到二进制格式
 * 4. 添加帧头（大端序长度）
 * 5. 追加到发送缓冲区
 *
 * @note 此方法不发送数据，仅将数据加入发送队列
 * @note 实际发送由 FlushWrites() 完成
 */
bool Connection::QueueResponse(const rpc::RpcResponse& response,
                               std::string* error_msg) {
  // 获取序列化后的消息体大小
  const std::size_t body_length =
      static_cast<std::size_t>(response.ByteSizeLong());

  // 检查帧大小合法性
  // 空消息或超过最大限制的消息都是非法的
  if (body_length == 0 || body_length > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid response frame size";
    }
    return false;
  }

  // 分配序列化缓冲区并序列化响应
  std::string body;
  body.resize(body_length);
  if (!response.SerializeToArray(body.data(), static_cast<int>(body.size()))) {
    if (error_msg != nullptr) {
      *error_msg = "failed to serialize RpcResponse";
    }
    return false;
  }

  // 编码帧（添加帧头）
  std::string frame;
  if (!EncodeFrame(body, &frame, error_msg)) {
    return false;
  }

  // 追加到发送缓冲区
  write_buffer_.append(frame);
  return true;
}

// ============================================================================
// 数据发送实现
// ============================================================================

/**
 * @brief 刷新发送缓冲区实现
 *
 * 将 write_buffer_ 中的所有数据通过 socket 发送到客户端。
 *
 * 实现细节：
 * - 使用 send() 系统调用发送数据
 * - 处理部分发送情况（循环发送直到完成）
 * - 使用 MSG_NOSIGNAL 防止对端关闭时触发 SIGPIPE
 * - 处理 EINTR 信号中断（自动重试）
 *
 * @note 发送完成后会清空已发送的数据
 */
bool Connection::FlushWrites(std::string* error_msg) {
  std::size_t sent = 0;  // 已发送字节数

  // 循环发送直到所有数据发送完成
  while (sent < write_buffer_.size()) {
    // 调用 send 发送数据
    // MSG_NOSIGNAL: 对端关闭时不发送 SIGPIPE 信号（避免进程崩溃）
    const ssize_t rc = ::send(fd_.Get(), write_buffer_.data() + sent,
                              write_buffer_.size() - sent, MSG_NOSIGNAL);

    if (rc > 0) {
      // 成功发送 rc 字节
      sent += static_cast<std::size_t>(rc);
      continue;
    }

    // rc <= 0 表示发送出错
    if (rc < 0 && errno == EINTR) {
      // 被信号中断，重试发送
      continue;
    }

    // 其他发送错误
    if (error_msg != nullptr) {
      *error_msg = std::string("send failed: ") + std::strerror(errno);
    }
    return false;
  }

  // 清空已发送的数据
  if (sent > 0) {
    write_buffer_.erase(0, sent);
  }
  return true;
}

// ============================================================================
// 帧编码/解码实现
// ============================================================================

/**
 * @brief 编码协议帧实现
 *
 * 将消息体封装为完整的协议帧。
 *
 * 帧格式：
 * ```
 * +----------------+------------------+
 * | 4 bytes header | N bytes body     |
 * +----------------+------------------+
 *      ^                                ^
 *      |                                |
 *   大端序长度                        原始数据
 *   (body.size())                    (不变)
 * ```
 *
 * @note 帧头使用大端序（网络字节序）确保跨平台兼容性
 */
bool Connection::EncodeFrame(const std::string& body, std::string* frame,
                             std::string* error_msg) {
  // 参数检查
  if (frame == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "frame output is null";
    }
    return false;
  }

  // 帧体大小合法性检查
  if (body.empty() || body.size() > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid frame body size";
    }
    return false;
  }

  // 将长度转换为大端序（网络字节序）
  // htonl: host to network long，将主机字节序转换为网络字节序
  const std::uint32_t be_length =
      htonl(static_cast<std::uint32_t>(body.size()));

  // 组装完整帧：帧头(4字节) + 帧体
  frame->clear();
  frame->reserve(kFrameHeaderBytes + body.size());  // 预分配空间
  // 添加帧头（大端序长度）
  frame->append(reinterpret_cast<const char*>(&be_length), kFrameHeaderBytes);
  // 添加帧体（原始数据）
  frame->append(body);
  return true;
}

/**
 * @brief 解码协议帧头实现
 *
 * 从缓冲区指定位置读取帧头并解析消息体长度。
 *
 * 解码流程：
 * 1. 从缓冲区读取 4 字节帧头
 * 2. 将大端序长度转换为主机字节序
 * 3. 验证长度合法性（非零且不超过最大限制）
 *
 * @note 此方法只解析帧头，不验证是否有足够的帧体数据
 */
bool Connection::DecodeFrameHeader(const std::string& buffer,
                                   std::size_t offset, std::size_t* body_length,
                                   std::string* error_msg) {
  // 参数检查
  if (body_length == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = "body_length is null";
    }
    return false;
  }

  // 读取 4 字节帧头
  std::uint32_t be_length = 0;
  std::memcpy(&be_length, buffer.data() + offset, kFrameHeaderBytes);

  // 将大端序转换为主机字节序
  // ntohl: network to host long，将网络字节序转换为主机字节序
  const std::uint32_t parsed = ntohl(be_length);

  // 验证长度合法性
  // 长度为 0 或超过最大限制都是非法的
  if (parsed == 0 || parsed > kMaxFrameSize) {
    if (error_msg != nullptr) {
      *error_msg = "invalid request frame length";
    }
    return false;
  }

  *body_length = static_cast<std::size_t>(parsed);
  return true;
}

}  // namespace rpc::server
