#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include "calc.pb.h"
#include "server/rpc_server.h"
#include "server/service_registry.h"

int main() {
  rpc::server::ServiceRegistry registry;

  const bool registered = registry.Register(
      "CalcService", "Add",
      [](std::string_view request_payload) -> std::string {
        calc::AddRequest request;
        if (!request.ParseFromArray(request_payload.data(),
                                    static_cast<int>(request_payload.size()))) {
          throw rpc::server::RpcError(rpc::server::RpcStatusCode::kParseError,
                                      "failed to parse calc::AddRequest");
        }

        calc::AddResponse response;
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
    std::cerr << "[ERROR] failed to register CalcService.Add\n";
    return 1;
  }

  constexpr std::uint16_t kPort = 50051;
  rpc::server::RpcServer server(kPort, registry);
  if (!server.Start()) {
    std::cerr << "[ERROR] server start failed\n";
    return 1;
  }

  return 0;
}
