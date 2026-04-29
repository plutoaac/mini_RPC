#include "common/rpc_error.h"

#include "rpc.pb.h"

namespace rpc::common {

int ToProtoErrorCode(const std::error_code& code) {
  if (!code) {
    return rpc::OK;
  }
  if (code.category() != GetRpcErrorCategory()) {
    return rpc::INTERNAL_ERROR;
  }
  const auto rpc_code = static_cast<ErrorCode>(code.value());

  switch (rpc_code) {
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

std::error_code FromProtoErrorCode(int code) {
  switch (code) {
    case rpc::OK:
      return make_error_code(ErrorCode::kOk);
    case rpc::METHOD_NOT_FOUND:
      return make_error_code(ErrorCode::kMethodNotFound);
    case rpc::PARSE_ERROR:
      return make_error_code(ErrorCode::kParseError);
    case rpc::INTERNAL_ERROR:
      return make_error_code(ErrorCode::kInternalError);
    default:
      return make_error_code(ErrorCode::kInternalError);
  }
}

}  // namespace rpc::common
