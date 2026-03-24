#include "client/pending_calls.h"

#include <chrono>

namespace rpc::client {

bool PendingCalls::Add(std::string request_id) {
  if (request_id.empty()) {
    return false;
  }

  std::scoped_lock lock(mutex_);
  return slots_.try_emplace(std::move(request_id)).second;
}

bool PendingCalls::Complete(std::string_view request_id, RpcCallResult result) {
  std::scoped_lock lock(mutex_);
  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end()) {
    return false;
  }

  it->second.done = true;
  it->second.result = std::move(result);
  it->second.cv.notify_all();
  return true;
}

std::optional<RpcCallResult> PendingCalls::WaitAndPop(
    std::string_view request_id, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);

  const auto key = std::string(request_id);
  auto it = slots_.find(key);
  if (it == slots_.end()) {
    return std::nullopt;
  }

  auto& slot = it->second;
  if (!slot.done) {
    const bool ready =
        slot.cv.wait_for(lock, timeout, [&slot] { return slot.done; });
    if (!ready) {
      return std::nullopt;
    }
    it = slots_.find(key);
    if (it == slots_.end()) {
      return std::nullopt;
    }
  }

  RpcCallResult result = std::move(it->second.result);
  slots_.erase(it);
  return result;
}

std::optional<RpcCallResult> PendingCalls::TryPop(std::string_view request_id) {
  std::scoped_lock lock(mutex_);

  const auto it = slots_.find(std::string(request_id));
  if (it == slots_.end() || !it->second.done) {
    return std::nullopt;
  }

  RpcCallResult result = std::move(it->second.result);
  slots_.erase(it);
  return result;
}

void PendingCalls::Remove(std::string_view request_id) {
  std::scoped_lock lock(mutex_);
  slots_.erase(std::string(request_id));
}

void PendingCalls::FailAll(const RpcCallResult& result_template) {
  std::scoped_lock lock(mutex_);
  for (auto& [_, slot] : slots_) {
    slot.done = true;
    slot.result = result_template;
    slot.cv.notify_all();
  }
}

std::size_t PendingCalls::Size() const {
  std::scoped_lock lock(mutex_);
  return slots_.size();
}

}  // namespace rpc::client
