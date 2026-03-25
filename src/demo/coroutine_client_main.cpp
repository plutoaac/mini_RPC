#include <cstdint>
#include <iostream>
#include <string>

#include "calc.pb.h"
#include "client/rpc_client.h"
#include "common/log.h"
#include "coroutine/task.h"

namespace {

rpc::coroutine::Task<void> RunDemo(rpc::client::RpcClient& client,
                                   const std::string& request_payload) {
  const auto result =
      co_await client.CallCo("CalcService", "Add", request_payload);

  if (!result.ok()) {
    rpc::common::LogError(
        "rpc failed: code=" + std::to_string(result.status.code.value()) +
        " category=" + result.status.code.category().name() +
        " msg=" + result.status.message);
    co_return;
  }

  calc::AddResponse response;
  if (!response.ParseFromString(result.response_payload)) {
    rpc::common::LogError("failed to parse AddResponse");
    co_return;
  }

  std::cout << "[co] Add(1,2) = " << response.result() << '\n';
}

}  // namespace

int main() {
  rpc::client::RpcClient client("127.0.0.1", 50051);

  calc::AddRequest request;
  request.set_a(1);
  request.set_b(2);

  std::string request_payload;
  if (!request.SerializeToString(&request_payload)) {
    rpc::common::LogError("failed to serialize AddRequest");
    return 1;
  }

  rpc::coroutine::SyncWait(RunDemo(client, request_payload));
  return 0;
}
