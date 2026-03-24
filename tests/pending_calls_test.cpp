#include "client/pending_calls.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "common/rpc_error.h"

namespace {

rpc::client::RpcCallResult MakeOk(std::string payload) {
  return {{rpc::common::make_error_code(rpc::common::ErrorCode::kOk), ""},
          std::move(payload)};
}

void TestAddCompletePop() {
  rpc::client::PendingCalls pending;
  assert(pending.Add("1"));
  assert(!pending.Add("1"));

  assert(pending.Complete("1", MakeOk("abc")));
  const auto result = pending.WaitAndPop("1", std::chrono::milliseconds(1));
  assert(result.has_value());
  assert(result->ok());
  assert(result->response_payload == "abc");
  assert(!pending.TryPop("1").has_value());
}

void TestMissingCompletion() {
  rpc::client::PendingCalls pending;
  assert(!pending.Complete("not-exist", MakeOk("x")));
  assert(!pending.TryPop("not-exist").has_value());
}

void TestFailAll() {
  rpc::client::PendingCalls pending;
  assert(pending.Add("a"));
  assert(pending.Add("b"));

  rpc::client::RpcCallResult fail{
      {rpc::common::make_error_code(rpc::common::ErrorCode::kInternalError),
       "forced fail"},
      {}};
  pending.FailAll(fail);

  const auto a = pending.WaitAndPop("a", std::chrono::milliseconds(1));
  const auto b = pending.WaitAndPop("b", std::chrono::milliseconds(1));
  assert(a.has_value() && b.has_value());
  assert(!a->ok() && !b->ok());
  assert(a->status.message == "forced fail");
}

void TestConcurrentAddCompletePop() {
  rpc::client::PendingCalls pending;
  constexpr int kN = 200;

  std::vector<std::thread> workers;
  workers.reserve(4);

  workers.emplace_back([&pending]() {
    for (int i = 0; i < kN; ++i) {
      const std::string id = std::to_string(i);
      const bool ok = pending.Add(id);
      assert(ok);
    }
  });

  workers.emplace_back([&pending]() {
    for (int i = 0; i < kN; ++i) {
      const std::string id = std::to_string(i);
      while (!pending.Complete(id, MakeOk("v" + id))) {
        std::this_thread::yield();
      }
    }
  });

  workers.emplace_back([&pending]() {
    int popped = 0;
    while (popped < kN) {
      const std::string id = std::to_string(popped);
      if (auto res = pending.WaitAndPop(id, std::chrono::milliseconds(1));
          res.has_value()) {
        assert(res->ok());
        ++popped;
      } else {
        std::this_thread::yield();
      }
    }
  });

  workers.emplace_back([&pending]() {
    // 仅用于并发读压测
    for (int i = 0; i < 1000; ++i) {
      (void)pending.Size();
    }
  });

  for (auto& th : workers) {
    th.join();
  }

  assert(pending.Size() == 0);
}

}  // namespace

int main() {
  TestAddCompletePop();
  TestMissingCompletion();
  TestFailAll();
  TestConcurrentAddCompletePop();

  std::cout << "pending_calls_test passed\n";
  return 0;
}
