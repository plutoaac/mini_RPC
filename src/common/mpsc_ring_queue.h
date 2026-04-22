#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace rpc::common::detail {

// Bounded MPSC ring queue specialized for logging style workloads.
//
// Design notes:
// 1. Producers claim a position by CAS on enqueue_pos_.
// 2. Each slot has a sequence number for state transitions:
//    - sequence == pos: slot is free for producer at logical position pos
//    - sequence == pos + 1: slot has published data for consumer
//    - after consume, consumer sets sequence = pos + Capacity (slot free again)
// 3. Consumer is single-threaded, so dequeue_pos_ is plain size_t.
//
// Memory ordering:
// - Producer stores payload, then sequence.store(pos + 1, release)
// - Consumer reads sequence with acquire before reading payload
// - Consumer destroys payload, then sequence.store(pos + Capacity, release)
// - Producer reads sequence with acquire before constructing into slot
//
// This keeps visibility guarantees explicit while avoiding locks in hot paths.
template <typename T, std::size_t Capacity>
class MpscRingQueue final {
  static_assert(Capacity >= 2, "Capacity must be >= 2");
  static_assert(std::has_single_bit(Capacity),
                "Capacity must be a power of two for mask indexing");
  static_assert(std::is_nothrow_move_constructible_v<T>,
                "T must be nothrow move constructible");
  static_assert(std::is_nothrow_move_assignable_v<T>,
                "T must be nothrow move assignable");

 public:
  MpscRingQueue() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpscRingQueue(const MpscRingQueue&) = delete;
  MpscRingQueue& operator=(const MpscRingQueue&) = delete;

  ~MpscRingQueue() noexcept { DrainAll(); }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  [[nodiscard]] bool TryPush(T&& value) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      Slot& slot = slots_[pos & kMask];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                 static_cast<std::intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(
                pos, pos + 1, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
          std::construct_at(ElementPtr(slot), std::move(value));
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
        continue;
      }

      if (diff < 0) {
        // Producer sees slot not yet released by consumer => queue full.
        return false;
      }

      // Another producer moved enqueue_pos_. Retry with newer position.
      pos = enqueue_pos_.load(std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::size_t PopBatch(std::span<T> out) noexcept {
    std::size_t count = 0;
    for (; count < out.size(); ++count) {
      if (!TryPopOne(out[count])) {
        break;
      }
    }
    return count;
  }

  [[nodiscard]] bool Empty() const noexcept {
    const std::size_t pos = dequeue_pos_;
    const Slot& slot = slots_[pos & kMask];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                               static_cast<std::intptr_t>(pos + 1);
    return diff < 0;
  }

 private:
  static constexpr std::size_t kMask = Capacity - 1;
  static constexpr std::size_t kCacheLine = 64;

  struct alignas(kCacheLine) Slot {
    std::atomic<std::size_t> sequence{0};
    alignas(T) std::byte storage[sizeof(T)]{};
  };

  [[nodiscard]] static T* ElementPtr(Slot& slot) noexcept {
    return std::launder(reinterpret_cast<T*>(slot.storage));
  }

  [[nodiscard]] static const T* ElementPtr(const Slot& slot) noexcept {
    return std::launder(reinterpret_cast<const T*>(slot.storage));
  }

  [[nodiscard]] bool TryPopOne(T& out) noexcept {
    Slot& slot = slots_[dequeue_pos_ & kMask];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                               static_cast<std::intptr_t>(dequeue_pos_ + 1);

    if (diff < 0) {
      return false;
    }

    T* elem = ElementPtr(slot);
    out = std::move(*elem);
    std::destroy_at(elem);

    // Release the slot for next producer wrap-around position.
    slot.sequence.store(dequeue_pos_ + Capacity, std::memory_order_release);
    ++dequeue_pos_;
    return true;
  }

  void DrainAll() noexcept {
    while (true) {
      Slot& slot = slots_[dequeue_pos_ & kMask];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                                 static_cast<std::intptr_t>(dequeue_pos_ + 1);
      if (diff < 0) {
        break;
      }

      std::destroy_at(ElementPtr(slot));
      slot.sequence.store(dequeue_pos_ + Capacity, std::memory_order_release);
      ++dequeue_pos_;
    }
  }

  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(kCacheLine) std::size_t dequeue_pos_{0};
  alignas(kCacheLine) std::array<Slot, Capacity> slots_{};
};

}  // namespace rpc::common::detail
