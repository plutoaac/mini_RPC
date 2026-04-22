#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

template <typename T>
class SPSCQueue {
 public:
  explicit SPSCQueue(size_t capacity)
      : capacity_(capacity + 1),  // 留一个空位区分满/空
        buffer_(capacity_) {}

  bool push(const T& value) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t next = increment(tail);

    // 读 head，判断是否满
    if (next == head_.load(std::memory_order_acquire)) {
      return false;  // full
    }

    buffer_[tail] = value;

    // 发布写入结果
    tail_.store(next, std::memory_order_release);
    return true;
  }

  bool push(T&& value) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t next = increment(tail);

    if (next == head_.load(std::memory_order_acquire)) {
      return false;
    }

    buffer_[tail] = std::move(value); //A
    tail_.store(next, std::memory_order_release);
    return true;
  }

  std::optional<T> pop() {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next = increment(head);
    // 读 tail，判断是否空
    if (head == tail_.load(std::memory_order_acquire)) {
      return std::nullopt;  // empty  B
    }

    T value = std::move(buffer_[head]);

    // 发布消费结果
    head_.store(next, std::memory_order_release);
    return value;
  }

 private:
  size_t increment(size_t idx) const { return (idx + 1) % capacity_; }

 private:
  const size_t capacity_;
  std::vector<T> buffer_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
};

/*

生产者一旦通过 acquire 看到了新的
head，就知道消费者已经完成了对旧槽位的消费，这个槽位现在可以安全复用。

*/

/*

消费者一旦通过 acquire 看到了新的 tail，就保证也能看到生产者在 release
之前写入的 buffer 数据。
*/