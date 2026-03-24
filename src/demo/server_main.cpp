#include <cstdint>
#include <string>
#include <string_view>

#include "calc.pb.h"
#include "common/log.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

int main() {
  // 1) 构建服务注册表。
  rpc::server::ServiceRegistry registry;

  // 2) 注册 CalcService.Add 业务 handler。
  const bool registered = registry.Register(
      "CalcService", "Add",
      [](std::string_view request_payload) -> std::string {
        calc::AddRequest request;
        // 框架层传过来的是 bytes，这里由业务层自己反序列化。
        if (!request.ParseFromArray(request_payload.data(),
                                    static_cast<int>(request_payload.size()))) {
          throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                      "failed to parse calc::AddRequest");
        }

        calc::AddResponse response;
        // 真正业务逻辑：执行加法。
        response.set_result(request.a() + request.b());

        std::string response_payload;
        if (!response.SerializeToString(&response_payload)) {
          throw rpc::server::RpcError(
              rpc::server::RpcStatusCode::kInternalError,
              "failed to serialize calc::AddResponse");
        }

        return response_payload;
      });

  if (!registered) {
    rpc::common::LogError("failed to register CalcService.Add");
    return 1;
  }

  constexpr std::uint16_t kPort = 50051;
  // 3) 启动 RPC 服务端循环。
  rpc::server::RpcServer server(kPort, registry);
  if (!server.Start()) {
    rpc::common::LogError("server start failed");
    return 1;
  }

  return 0;
}
