#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "calc.pb.h"
#include "client/rpc_client.h"

namespace {

bool WaitServerReady(const std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  rpc::client::RpcClient probe(
      "127.0.0.1", 50051,
      {.send_timeout = std::chrono::milliseconds(200),
       .recv_timeout = std::chrono::milliseconds(200)});

  while (std::chrono::steady_clock::now() - start < timeout) {
    if (probe.Connect()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const int iterations = (argc > 1) ? std::max(1, std::atoi(argv[1])) : 1000;

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::cerr << "fork failed\n";
    return 1;
  }

  if (pid == 0) {
    ::execl("./rpc_server_demo", "./rpc_server_demo", nullptr);
    _exit(127);
  }

  if (!WaitServerReady(std::chrono::seconds(2))) {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    std::cerr << "benchmark failed: server not ready\n";
    return 1;
  }

  rpc::client::RpcClient client(
      "127.0.0.1", 50051,
      {.send_timeout = std::chrono::milliseconds(3000),
       .recv_timeout = std::chrono::milliseconds(3000)});

  calc::AddRequest add_req;
  add_req.set_a(1);
  add_req.set_b(2);

  std::string payload;
  if (!add_req.SerializeToString(&payload)) {
    std::cerr << "serialize request failed\n";
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    return 1;
  }

  std::vector<long long> lat_us;
  lat_us.reserve(static_cast<std::size_t>(iterations));

  for (int i = 0; i < iterations; ++i) {
    const auto t1 = std::chrono::steady_clock::now();
    const auto res = client.Call("CalcService", "Add", payload);
    const auto t2 = std::chrono::steady_clock::now();

    if (!res.ok()) {
      std::cerr << "rpc call failed at iteration " << i
                << ", code=" << res.status.code.value()
                << ", msg=" << res.status.message << '\n';
      ::kill(pid, SIGTERM);
      ::waitpid(pid, nullptr, 0);
      return 1;
    }

    lat_us.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());
  }

  std::sort(lat_us.begin(), lat_us.end());
  const long long sum_us = std::accumulate(lat_us.begin(), lat_us.end(), 0LL);
  const double avg_us = static_cast<double>(sum_us) / lat_us.size();
  const auto p95_idx = static_cast<std::size_t>(lat_us.size() * 0.95);
  const long long p95_us = lat_us[std::min(p95_idx, lat_us.size() - 1)];
  const double throughput_qps =
      (1e6 * lat_us.size()) / static_cast<double>(sum_us);

  std::cout << "RPC Benchmark (single connection)\n";
  std::cout << "iterations=" << iterations << '\n';
  std::cout << "avg_latency_us=" << avg_us << '\n';
  std::cout << "p95_latency_us=" << p95_us << '\n';
  std::cout << "throughput_qps=" << throughput_qps << '\n';

  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);

  return 0;
}
