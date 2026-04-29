#pragma once

#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace rpc::common {

// 框架内部统一错误码：服务端、客户端、业务异常都复用这一套。
enum class ErrorCode {
  kOk = 0,
  kMethodNotFound = 1,
  kParseError = 2,
  kInternalError = 3,
};

class RpcErrorCategory final : public std::error_category {
 public:
  const char* name() const noexcept override { return "rpc"; }

  std::string message(int ev) const override {
    switch (static_cast<ErrorCode>(ev)) {
      case ErrorCode::kOk:
        return "ok";
      case ErrorCode::kMethodNotFound:
        return "method not found";
      case ErrorCode::kParseError:
        return "parse error";
      case ErrorCode::kInternalError:
        return "internal error";
      default:
        return "unknown rpc error";
    }
  }
};

[[nodiscard]] inline const std::error_category& GetRpcErrorCategory() {
  static const RpcErrorCategory category;
  return category;
}

[[nodiscard]] inline std::error_code make_error_code(ErrorCode code) {
  return {static_cast<int>(code), GetRpcErrorCategory()};
}

[[nodiscard]] inline const char* ToString(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return "OK";
    case ErrorCode::kMethodNotFound:
      return "METHOD_NOT_FOUND";
    case ErrorCode::kParseError:
      return "PARSE_ERROR";
    case ErrorCode::kInternalError:
      return "INTERNAL_ERROR";
    default:
      return "UNKNOWN";
  }
}

// 框架错误码 -> protobuf 错误码（用于网络传输）。
// 返回值类型为 int（底层等价于 rpc::ErrorCode），避免头文件依赖 rpc.pb.h。
[[nodiscard]] int ToProtoErrorCode(const std::error_code& code);

// protobuf 错误码 -> 框架错误码（用于客户端统一处理）。
// 参数类型为 int（底层等价于 rpc::ErrorCode），避免头文件依赖 rpc.pb.h。
[[nodiscard]] std::error_code FromProtoErrorCode(int code);

// 统一状态对象：替代 scattered 的 bool + error_msg 组合。
struct Status {
  std::error_code code{make_error_code(ErrorCode::kOk)};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return !code; }

  [[nodiscard]] static Status Ok() { return Status{}; }
};

// 框架异常：业务 handler 可直接抛出此异常，服务端统一映射为响应错误码。
class RpcException : public std::runtime_error {
 public:
  RpcException(std::error_code code, std::string message)
      : std::runtime_error(message.empty() ? code.message() : message),
        code_(std::move(code)) {}

  RpcException(ErrorCode code, std::string message)
      : RpcException(make_error_code(code), std::move(message)) {}

  [[nodiscard]] const std::error_code& code() const noexcept { return code_; }

 private:
  std::error_code code_;
};

}  // namespace rpc::common

namespace std {
template <>
struct is_error_code_enum<rpc::common::ErrorCode> : true_type {};
}  // namespace std
