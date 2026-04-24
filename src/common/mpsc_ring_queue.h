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

// ============================================================================
// MpscRingQueue —— 无锁多生产者单消费者（MPSC）环形队列
// ============================================================================
// 设计要点：
//   1. 生产者通过 CAS 原子操作竞争 enqueue_pos_ 来申请槽位。
//   2. 每个槽位维护一个 sequence 序号，形成三状态机：
//      - sequence == pos          : 空闲，等待位于逻辑位置 pos 的生产者写入
//      - sequence == pos + 1      : 生产者已发布数据，消费者可读
//      - sequence == pos + Capacity : 消费者已消费完毕，槽位再次空闲（支持环绕）
//   3. 消费者为单线程，dequeue_pos_ 使用普通 size_t 即可，无需原子操作。
//   4. 槽位按缓存行对齐（alignas(kCacheLine)），避免不同线程读写相邻槽位时的
//      伪共享（false sharing），提升并发性能。
//
// 内存序说明：
//   - 生产者写入 payload 后，以 release 语义将 sequence 置为 pos + 1；
//   - 消费者以 acquire 语义读取 sequence，确认可读后再读取 payload；
//   - 消费者销毁 payload 后，以 release 语义将 sequence 置为 pos + Capacity；
//   - 生产者以 acquire 语义读取 sequence，确认空闲后再在槽位上构造对象。
//   这种显式的内存序搭配，既保证了跨线程可见性，又避免了锁带来的热路径开销。
//
// 模板约束：
//   - Capacity >= 2，且必须是 2 的幂（用于掩码取模）；
//   - T 必须支持 noexcept 移动构造和 noexcept 移动赋值，确保无锁路径不会抛异常。
// ============================================================================

// 专用于日志类负载的定长 MPSC 环形队列。
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
  // --------------------------------------------------------------------------
  // 构造函数：初始化每个槽位的 sequence 为对应位置值，表示全部空闲。
  // --------------------------------------------------------------------------
  MpscRingQueue() noexcept {
    for (std::size_t i = 0; i < Capacity; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpscRingQueue(const MpscRingQueue&) = delete;
  MpscRingQueue& operator=(const MpscRingQueue&) = delete;

  // --------------------------------------------------------------------------
  // 析构函数：排空队列中剩余元素，避免内存泄漏。
  // --------------------------------------------------------------------------
  ~MpscRingQueue() noexcept { DrainAll(); }

  [[nodiscard]] static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

  // --------------------------------------------------------------------------
  // TryPush —— 生产者入队接口（支持多线程并发调用）
  // --------------------------------------------------------------------------
  // 流程：
  //   1. 读取当前 enqueue_pos_（relaxed）。
  //   2. 检查对应槽位的 sequence：
  //      - 若 sequence == pos，说明槽位空闲，尝试 CAS 将 enqueue_pos_ 推进到 pos+1；
  //        CAS 成功后，在槽位上 placement-new 构造对象，再以 release 发布 sequence。
  //      - 若 sequence < pos，说明消费者尚未释放该槽位（队列满），返回 false。
  //      - 若 sequence > pos，说明其他生产者已抢先推进了 enqueue_pos_，重新读取并重试。
  // --------------------------------------------------------------------------
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
        // 生产者发现该槽位尚未被消费者释放 => 队列已满，快速失败。
        return false;
      }

      // 其他生产者已经推进了 enqueue_pos_，用最新值重新尝试。
      pos = enqueue_pos_.load(std::memory_order_relaxed);
    }
  }

  // --------------------------------------------------------------------------
  // PopBatch —— 消费者批量出队接口（仅允许单线程调用）
  // --------------------------------------------------------------------------
  // 依次调用 TryPopOne，直到将 out 填满或队列空，返回实际弹出数量。
  // --------------------------------------------------------------------------
  [[nodiscard]] std::size_t PopBatch(std::span<T> out) noexcept {
    std::size_t count = 0;
    for (; count < out.size(); ++count) {
      if (!TryPopOne(out[count])) {
        break;
      }
    }
    return count;
  }

  // --------------------------------------------------------------------------
  // Empty —— 判断队列当前是否为空（消费者视角）
  // --------------------------------------------------------------------------
  // 读取 dequeue_pos_ 对应槽位的 sequence，若 seq < pos + 1 则说明无数据可读。
  // --------------------------------------------------------------------------
  [[nodiscard]] bool Empty() const noexcept {
    const std::size_t pos = dequeue_pos_;
    const Slot& slot = slots_[pos & kMask];
    const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
    const std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                               static_cast<std::intptr_t>(pos + 1);
    return diff < 0;
  }

 private:
  // 掩码：Capacity 为 2 的幂，故可用位与代替取模运算。
  static constexpr std::size_t kMask = Capacity - 1;
  // 主流 CPU 缓存行大小按 64 字节对齐。
  static constexpr std::size_t kCacheLine = 64;

  // --------------------------------------------------------------------------
  // Slot —— 队列槽位结构
  // --------------------------------------------------------------------------
  // 按缓存行对齐，sequence 与 storage 处于同一缓存行内（消费者读取时一起加载），
  // 但相邻槽位之间不会因读写 sequence 而产生伪共享。
  // storage 使用 std::byte 数组提供未初始化的原始内存，由 placement-new 管理对象生命周期。
  // --------------------------------------------------------------------------
  struct alignas(kCacheLine) Slot {
    std::atomic<std::size_t> sequence{0};    // 槽位状态序号
    alignas(T) std::byte storage[sizeof(T)]{}; // 对象存储区（未初始化内存）
  };

  // 将 Slot 的 storage 解释为 T*（配合 std::launder 符合严格别名规则）。
  [[nodiscard]] static T* ElementPtr(Slot& slot) noexcept {
    return std::launder(reinterpret_cast<T*>(slot.storage));
  }

  [[nodiscard]] static const T* ElementPtr(const Slot& slot) noexcept {
    return std::launder(reinterpret_cast<const T*>(slot.storage));
  }

  // --------------------------------------------------------------------------
  // TryPopOne —— 单条出队（消费者内部使用）
  // --------------------------------------------------------------------------
  // 流程：
  //   1. 读取 dequeue_pos_ 对应槽位的 sequence（acquire）。
  //   2. 若 seq < pos + 1，说明生产者尚未发布该槽位，返回 false。
  //   3. 否则，移动构造出对象、销毁原对象，再将 sequence 置为 pos + Capacity
  //      （release），表示该槽位已释放，可参与下一轮环绕复用。
  //   4. 推进 dequeue_pos_。
  // --------------------------------------------------------------------------
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

    // 将槽位释放给下一圈生产者使用（环绕复用）。
    slot.sequence.store(dequeue_pos_ + Capacity, std::memory_order_release);
    ++dequeue_pos_;
    return true;
  }

  // --------------------------------------------------------------------------
  // DrainAll —— 析构时排空队列，销毁所有残留元素并释放槽位
  // --------------------------------------------------------------------------
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

  // --------------------------------------------------------------------------
  // 成员变量（均按缓存行对齐，防止伪共享）
  // --------------------------------------------------------------------------

  // 生产者共享的写位置，多线程通过 CAS 竞争。
  alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
  // 消费者独占的读位置，单线程无需原子操作。
  alignas(kCacheLine) std::size_t dequeue_pos_{0};
  // 定长槽位数组，构成环形缓冲区。
  alignas(kCacheLine) std::array<Slot, Capacity> slots_{};
};

}  // namespace rpc::common::detail
