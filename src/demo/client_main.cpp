#include <cstdint>
#include <iostream>
#include <string>

#include "calc.pb.h"
#include "client/rpc_client.h"

int main() {
  rpc::client::RpcClient client("127.0.0.1", 50051);

  calc::AddRequest request;
  request.set_a(1);
  request.set_b(2);

  std::string request_payload;
  if (!request.SerializeToString(&request_payload)) {
    std::cerr << "[ERROR] failed to serialize AddRequest\n";
    return 1;
  }

  const auto result = client.Call("CalcService", "Add", request_payload);
  if (!result.ok()) {
    std::cerr << "[ERROR] rpc failed: code="
              << static_cast<int>(result.status.code)
              << " msg=" << result.status.message << '\n';
    return 1;
  }

  calc::AddResponse response;
  if (!response.ParseFromString(result.response_payload)) {
    std::cerr << "[ERROR] failed to parse AddResponse\n";
    return 1;
  }

  std::cout << "Add(1,2) = " << response.result() << '\n';
  return 0;
}
