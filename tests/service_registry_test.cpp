#include "server/service_registry.h"

#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

void TestRegisterAndFind() {
  rpc::server::ServiceRegistry registry;
  const bool ok =
      registry.Register("CalcService", "Add",
                        [](std::string_view req) { return std::string(req); });
  assert(ok);

  const auto handler = registry.Find("CalcService", "Add");
  assert(handler.has_value());
  assert(handler->get()("abc") == "abc");
}

void TestDuplicateRegisterFails() {
  rpc::server::ServiceRegistry registry;
  assert(registry.Register("CalcService", "Add",
                           [](std::string_view) { return std::string("1"); }));
  assert(!registry.Register("CalcService", "Add",
                            [](std::string_view) { return std::string("2"); }));
}

void TestFindMissing() {
  rpc::server::ServiceRegistry registry;
  const auto handler = registry.Find("NoService", "NoMethod");
  assert(!handler.has_value());
}

void TestConcurrentRegisterAndFind() {
  rpc::server::ServiceRegistry registry;

  constexpr int kThreadCount = 8;
  constexpr int kMethodsPerThread = 100;

  std::vector<std::thread> workers;
  workers.reserve(kThreadCount);

  for (int t = 0; t < kThreadCount; ++t) {
    workers.emplace_back([&registry, t]() {
      for (int i = 0; i < kMethodsPerThread; ++i) {
        const std::string service = "S" + std::to_string(t);
        const std::string method = "M" + std::to_string(i);
        const bool ok = registry.Register(
            service, method,
            [](std::string_view in) { return std::string(in); });
        assert(ok);
      }
    });
  }

  for (auto& th : workers) {
    th.join();
  }

  workers.clear();
  for (int t = 0; t < kThreadCount; ++t) {
    workers.emplace_back([&registry, t]() {
      for (int i = 0; i < kMethodsPerThread; ++i) {
        const std::string service = "S" + std::to_string(t);
        const std::string method = "M" + std::to_string(i);
        const auto handler = registry.Find(service, method);
        assert(handler.has_value());
        assert(handler->get()("x") == "x");
      }
    });
  }

  for (auto& th : workers) {
    th.join();
  }
}

}  // namespace

int main() {
  TestRegisterAndFind();
  TestDuplicateRegisterFails();
  TestFindMissing();
  TestConcurrentRegisterAndFind();

  std::cout << "service_registry_test passed\n";
  return 0;
}
