/**
 * @file b.cpp
 * @brief 多生产者多消费者无锁队列（MPMC Queue）
 *
 * 基于 Dmitry Vyukov 的高性能有界 MPMC 队列算法实现。
 * 使用 sequence 号机制协调多线程访问，避免数据竞争。
 *
 * ## 核心设计
 *
 * 1. **Sequence 号机制**
 *    每个 slot 有一个 sequence 号，用于判断 slot 的状态：
 *    - sequence == pos：slot 可写（生产者）
 *    - sequence == pos + 1：slot 可读（消费者）
 *    - sequence == pos + capacity：slot 进入下一轮，可再次写入
 *
 * 2. **CAS 原子操作**
 *    使用 compare_exchange_weak 原子地抢占位置，处理多线程竞争。
 *
 * 3. **内存序**
 *    - relaxed：读取自己的位置指针
 *    - acquire：读取 slot 的 sequence
 *    - release：更新 slot 的 sequence
 *
 * 4. **缓存行对齐**
 *    使用 alignas(64) 避免 false sharing，提升多核性能。
 *
 * ## 使用示例
 *
 * @code
 *   MPMCQueue<int> queue(1024);
 *
 *   // 生产者线程
 *   queue.push(42);
 *
 *   // 消费者线程
 *   auto value = queue.pop();
 *   if (value) {
 *     std::cout << *value << std::endl;
 *   }
 * @endcode
 *
 * @author RPC Framework Team
 * @date 2024
 */

#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <vector>

template <typename T>
class MPMCQueue {
  // 确保 T 的移动或拷贝不会抛出异常
  // 否则在 push/pop 过程中抛出异常会导致队列状态不一致
  static_assert(std::is_nothrow_move_constructible_v<T> ||
                    std::is_nothrow_copy_constructible_v<T>,
                "T must be nothrow movable or copyable");

 public:
  /**
   * @brief 构造函数
   *
   * @param capacity 队列容量（会被向上取整到 2 的幂）
   *
   * 初始化步骤：
   * 1. 将容量取整到 2 的幂（用于位运算优化取模）
   * 2. 分配 buffer 和 slots
   * 3. 初始化每个 slot 的 sequence = 索引值
   *    表示 slot i 在第 0 轮可写
   */
  explicit MPMCQueue(size_t capacity)
      : capacity_(RoundToPowerOfTwo(capacity)),
        buffer_(capacity_),
        slots_(capacity_) {
    // 初始化 sequence：slot[i] 的初始 sequence = i
    // 表示该 slot 在第 0 轮可以被生产者写入
    // 当生产者写入后，sequence 变为 i+1，表示可被消费者读取
    for (size_t i = 0; i < capacity_; ++i) {
      slots_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  /**
   * @brief 入队（左值引用版本）
   *
   * @param value 要入队的值
   * @return true 入队成功
   * @return false 队列已满
   *
   * ## 算法流程
   *
   * 1. 读取当前入队位置 pos
   * 2. 检查对应 slot 的 sequence：
   *    - diff == 0：slot 可写，尝试 CAS 抢占
   *    - diff < 0：队列满
   *    - diff > 0：其他生产者已抢占，重新读取 pos
   * 3. CAS 成功后写入数据，更新 sequence
   */
  bool push(const T& value) {
    // relaxed 读取：只需要原子性，不需要同步
    // 因为如果 CAS 失败会自动更新 pos
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    while (true) {
      // 使用位运算计算 slot 索引（等价于 pos % capacity_）
      // 要求 capacity_ 必须是 2 的幂
      Slot* slot = &slots_[pos & (capacity_ - 1)];

      // acquire 读取：需要看到消费者 release 的 sequence 更新
      // 确保在读取 sequence 之后才读取 buffer
      size_t seq = slot->sequence.load(std::memory_order_acquire);

      // diff = sequence - pos
      // 用于判断 slot 的状态
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        // ┌─────────────────────────────────────────────────────────────┐
        // │ diff == 0：sequence == pos                                   │
        // │ 表示该 slot 在当前轮次可写                                    │
        // │                                                             │
        // │ 多个生产者可能同时到达这里，需要通过 CAS 竞争                  │
        // └─────────────────────────────────────────────────────────────┘

        // CAS：原子地尝试将 enqueue_pos_ 从 pos 更新为 pos+1
        // - 成功：获得该 slot 的写入权
        // - 失败：其他生产者抢先了，pos 会被更新为最新值
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed)) {
          // ┌─────────────────────────────────────────────────────────────┐
          // │ CAS 成功，获得写入权                                         │
          // │                                                             │
          // │ 此时：                                                       │
          // │ - enqueue_pos_ 已更新为 pos+1                               │
          // │ - 但 buffer 和 sequence 还没更新                             │
          // │ - 消费者会因为 sequence != pos+1 而等待                      │
          // └─────────────────────────────────────────────────────────────┘

          // 写入数据到 buffer
          buffer_[pos & (capacity_ - 1)] = value;

          // release 更新：保证 buffer 写入对消费者可见
          // sequence = pos + 1 表示数据已就绪，消费者可以读取
          // 消费者通过 acquire 读取 sequence 后，能看到 buffer 的内容
          slot->sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS 失败，pos 已被更新为最新值，继续循环重试
      } else if (diff < 0) {
        // ┌─────────────────────────────────────────────────────────────┐
        // │ diff < 0：sequence < pos                                     │
        // │                                                             │
        // │ 表示消费者还没消费完这个 slot（sequence 还没更新到 pos+capacity）│
        // │ 队列已满，无法写入                                           │
        // └─────────────────────────────────────────────────────────────┘
        return false;
      } else {
        // ┌─────────────────────────────────────────────────────────────┐
        // │ diff > 0：sequence > pos                                     │
        // │                                                             │
        // │ 表示 enqueue_pos_ 已被其他生产者推进                          │
        // │ 需要重新读取最新的 pos                                        │
        // └─────────────────────────────────────────────────────────────┘
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  /**
   * @brief 入队（右值引用版本）
   *
   * @param value 要入队的值（通过移动语义）
   * @return true 入队成功
   * @return false 队列已满
   */
  bool push(T&& value) {
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    while (true) {
      Slot* slot = &slots_[pos & (capacity_ - 1)];
      size_t seq = slot->sequence.load(std::memory_order_acquire);
      intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed)) {
          buffer_[pos & (capacity_ - 1)] = std::move(value);
          slot->sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
        // CAS 失败，pos 已更新，继续循环
      } else if (diff < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  /**
   * @brief 出队
   *
   * @return std::optional<T> 成功返回值，失败返回 nullopt（队列空）
   *
   * ## 算法流程
   *
   * 1. 读取当前出队位置 pos
   * 2. 检查对应 slot 的 sequence：
   *    - diff == 0：slot 可读，尝试 CAS 抢占
   *    - diff < 0：队列空
   *    - diff > 0：其他消费者已抢占，重新读取 pos
   * 3. CAS 成功后读取数据，更新 sequence
   */
  std::optional<T> pop() {
    // relaxed 读取：只需要原子性
    size_t pos = dequeue_pos_.load(std::memory_order_relaxed);

    while (true) {
      Slot* slot = &slots_[pos & (capacity_ - 1)];

      // acquire 读取：需要看到生产者 release 的 sequence 更新
      // 确保在读取 sequence 之后才读取 buffer
      size_t seq = slot->sequence.load(std::memory_order_acquire);

      // 注意：消费者检查的是 sequence == pos + 1
      // 因为生产者写入后会设置 sequence = pos + 1
      intptr_t diff =
          static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

      if (diff == 0) {
        // ┌─────────────────────────────────────────────────────────────┐
        // │ diff == 0：sequence == pos + 1                               │
        // │ 表示该 slot 数据已就绪，可以被读取                            │
        // │                                                             │
        // │ 多个消费者可能同时到达这里，需要通过 CAS 竞争                  │
        // └─────────────────────────────────────────────────────────────┘

        // CAS：原子地尝试将 dequeue_pos_ 从 pos 更新为 pos+1
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1,
                                               std::memory_order_relaxed)) {
          // ┌─────────────────────────────────────────────────────────────┐
          // │ CAS 成功，获得读取权                                         │
          // │                                                             │
          // │ 此时可以安全地读取 buffer                                    │
          // │ 因为生产者的 release 已经保证了数据的可见性                   │
          // └─────────────────────────────────────────────────────────────┘

          // 读取数据
          T value = std::move(buffer_[pos & (capacity_ - 1)]);

          // release 更新：设置 sequence = pos + capacity_
          // 表示该 slot 进入下一轮，可以被生产者再次写入
          // 下一个生产者会检查 sequence == pos + capacity（即新一轮的 pos）
          slot->sequence.store(pos + capacity_, std::memory_order_release);
          return value;
        }
        // CAS 失败，pos 已更新，继续循环
      } else if (diff < 0) {
        // ┌─────────────────────────────────────────────────────────────┐
        // │ diff < 0：sequence < pos + 1                                 │
        // │                                                             │
        // │ 表示生产者还没写入这个 slot                                   │
        // │ 队列为空，无法读取                                           │
        // └─────────────────────────────────────────────────────────────┘
        return std::nullopt;
      } else {
        // diff > 0：dequeue_pos_ 已被其他消费者推进
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

 private:
  /**
   * @brief 将数值向上取整到 2 的幂
   *
   * 用于优化取模运算为位运算：pos % capacity 变为 pos & (capacity - 1)
   *
   * @param n 输入数值
   * @return size_t 大于等于 n 的最小 2 的幂
   */
  static size_t RoundToPowerOfTwo(size_t n) {
    if (n == 0) return 1;
    // 如果已经是 2 的幂，直接返回
    if ((n & (n - 1)) == 0) return n;
    // 位操作技巧：将最高位以下的所有位都置 1，然后加 1
    --n;
    n |= n >> 1;   // 将最高 2 位置 1
    n |= n >> 2;   // 将最高 4 位置 1
    n |= n >> 4;   // 将最高 8 位置 1
    n |= n >> 8;   // 将最高 16 位置 1
    n |= n >> 16;  // 将最高 32 位置 1
    n |= n >> 32;  // 将最高 64 位置 1
    return n + 1;  // 加 1 得到 2 的幂
  }

  /**
   * @brief Slot 结构体
   *
   * 每个 slot 包含一个 sequence 号，用于协调生产者和消费者。
   *
   * ## Sequence 状态转换
   *
   * ```
   * 初始状态：sequence = i（slot 索引）
   *          表示 slot i 在第 0 轮可写
   *
   * 生产者写入后：sequence = pos + 1
   *          表示数据已就绪，消费者可读
   *
   * 消费者读取后：sequence = pos + capacity_
   *          表示该 slot 进入下一轮，可被生产者再次写入
   * ```
   *
   * ## 缓存行对齐
   *
   * 使用 alignas(64) 确保 sequence 独占一个缓存行，
   * 避免多线程访问不同 slot 时的 false sharing 问题。
   */
  struct Slot {
    alignas(64) std::atomic<size_t> sequence{0};  // 缓存行对齐，避免 false sharing
  };

  // ============ 成员变量 ============

  const size_t capacity_;          // 队列容量（总是 2 的幂）
  std::vector<T> buffer_;          // 数据存储缓冲区
  std::vector<Slot> slots_;        // 每个 slot 的 sequence 号

  // 入队位置指针（生产者使用）
  // alignas(64) 避免 enqueue_pos_ 和 dequeue_pos_ 在同一缓存行
  alignas(64) std::atomic<size_t> enqueue_pos_{0};

  // 出队位置指针（消费者使用）
  alignas(64) std::atomic<size_t> dequeue_pos_{0};
};

/*


┌─────────────────────────────────────────────────────────────────────┐
│                     为什么容量是 2 的幂                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1. 位运算优化：pos % capacity → pos & (capacity - 1)               │
│                                                                     │
│  2. 性能提升：取模 ~20-80 周期 → 位与 ~1 周期                        │
│                                                                     │
│  3. 前提条件：capacity 必须是 2 的幂                                 │
│                                                                     │
│  4. 自动处理：RoundToPowerOfTwo() 将任意容量向上取整                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

*/