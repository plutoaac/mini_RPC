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
/*


RunDemo 协程
    │
    │  co_await client.CallCo(...)
    │
    ▼
Task<RpcCallResult> task = CallCo(...)
    │
    │  task.operator co_await()  ← Task::Awaiter 被使用！
    │
    ▼
Task::Awaiter
    │
    ├── await_ready() → false（Task 未完成）
    │
    ├── await_suspend(RunDemo的句柄)
    │   │
    │   │  保存 continuation = RunDemo的句柄
    │   │
    │   ▼
    │  CallCo 协程内部：
    │      co_await FromFuture(CallAsync(...))
    │      │
    │      ▼
    │  FutureAwaiter
    │      │
    │      ├── await_ready() → false
    │      ├── await_suspend() → 启动后台线程等待 future
    │      │
    │      ▼
    │  后台线程：future.get() 完成
    │      │
    │      ▼
    │  handle.resume() → 恢复 CallCo
    │      │
    │      ▼
    │  CallCo: co_return 结果
    │      │
    │      ▼
    │  final_suspend() → continuation.resume()
    │      │
    │      └──────────────────────────────┐
    │                                    │
    ▼                                    ▼
RunDemo 协程恢复                      恢复 RunDemo
    │
    ▼
awaiter.await_resume() → 获取 RpcCallResult



*/