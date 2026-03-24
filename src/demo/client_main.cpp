#include <cstdint>
#include <iostream>
#include <string>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/log.h"

int main() {
  // 1) 创建客户端并指定目标地址。
  rpc::client::RpcClient client("127.0.0.1", 50051);

  // 2) 组装业务请求 Add(1, 2)。
  calc::AddRequest request;
  request.set_a(1);
  request.set_b(2);

  std::string request_payload;
  if (!request.SerializeToString(&request_payload)) {
    rpc::common::LogError("failed to serialize AddRequest");
    return 1;
  }

  // 3) 通过通用 RPC 接口发起调用。
  const auto result = client.Call("CalcService", "Add", request_payload);
  if (!result.ok()) {
    rpc::common::LogError(
        "rpc failed: code=" + std::to_string(result.status.code.value()) +
        " category=" + result.status.code.category().name() +
        " msg=" + result.status.message);
    return 1;
  }

  // 4) 业务层解析响应 payload。
  calc::AddResponse response;
  if (!response.ParseFromString(result.response_payload)) {
    rpc::common::LogError("failed to parse AddResponse");
    return 1;
  }

  std::cout << "Add(1,2) = " << response.result() << '\n';
  return 0;
}
