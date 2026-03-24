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

  std::string response_payload;
  int error_code = 0;
  std::string error_msg;

  const bool ok = client.Call("CalcService", "Add", request_payload,
                              &response_payload, &error_code, &error_msg);
  if (!ok) {
    std::cerr << "[ERROR] rpc failed: code=" << error_code
              << " msg=" << error_msg << '\n';
    return 1;
  }

  calc::AddResponse response;
  if (!response.ParseFromString(response_payload)) {
    std::cerr << "[ERROR] failed to parse AddResponse\n";
    return 1;
  }

  std::cout << "Add(1,2) = " << response.result() << '\n';
  return 0;
}
