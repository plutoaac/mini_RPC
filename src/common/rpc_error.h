#pragma once

#include <stdexcept>
#include <string>

#include "rpc.pb.h"

namespace rpc::common {

enum class ErrorCode {
  kOk = 0,
  kMethodNotFound = 1,
  kParseError = 2,
  kInternalError = 3,
};

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

[[nodiscard]] inline rpc::ErrorCode ToProtoErrorCode(ErrorCode code) {
  switch (code) {
    case ErrorCode::kOk:
      return rpc::OK;
    case ErrorCode::kMethodNotFound:
      return rpc::METHOD_NOT_FOUND;
    case ErrorCode::kParseError:
      return rpc::PARSE_ERROR;
    case ErrorCode::kInternalError:
      return rpc::INTERNAL_ERROR;
    default:
      return rpc::INTERNAL_ERROR;
  }
}

[[nodiscard]] inline ErrorCode FromProtoErrorCode(rpc::ErrorCode code) {
  switch (code) {
    case rpc::OK:
      return ErrorCode::kOk;
    case rpc::METHOD_NOT_FOUND:
      return ErrorCode::kMethodNotFound;
    case rpc::PARSE_ERROR:
      return ErrorCode::kParseError;
    case rpc::INTERNAL_ERROR:
      return ErrorCode::kInternalError;
    default:
      return ErrorCode::kInternalError;
  }
}

struct Status {
  ErrorCode code{ErrorCode::kOk};
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }

  [[nodiscard]] static Status Ok() { return Status{}; }
};

class RpcException : public std::runtime_error {
 public:
  RpcException(ErrorCode code, std::string message)
      : std::runtime_error(std::move(message)), code_(code) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

}  // namespace rpc::common
