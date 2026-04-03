#include <coroutine>
#include <exception>
#include <iostream>
#include <utility>

// 模板化的 Generator：可以产生任何类型的数据
template <typename T>
class Generator {
 public:
  // 提前声明 promise_type，这是 C++ 协程的强制规定
  struct promise_type;
  // 协程句柄，用于在外层控制协程的挂起和恢复
  using handle_type = std::coroutine_handle<promise_type>;

  // ==========================================
  // 1. 协程内部状态机的心脏：promise_type
  // ==========================================
  struct promise_type {
    T value_;                       // 用于临时保存 co_yield 吐出来的数据
    std::exception_ptr exception_;  // 异常捕获

    // 协程创建时，把控制权交给 Generator
    Generator get_return_object() {
      return Generator(handle_type::from_promise(*this));
    }

    // 惰性求值：协程刚创建时，先挂起，不直接执行
    std::suspend_always initial_suspend() { return {}; }
    // 协程自然结束后，保持挂起（为了让外层能安全读取最后的状态并手动销毁）
    std::suspend_always final_suspend() noexcept { return {}; }

    // 捕获协程内部抛出的异常
    void unhandled_exception() { exception_ = std::current_exception(); }

    // 💥 核心魔法：处理 co_yield value;
    std::suspend_always yield_value(T from) {
      value_ = from;  // 1. 保存数据
      return {};      // 2. 返回 suspend_always 让协程立刻挂起
    }

    // 💥 上一轮讲的核心闭环：处理函数的自然结束 (})
    void return_void() {}
  };

  // ==========================================
  // 2. 外部控制流与 RAII 资源管理
  // ==========================================
  handle_type h_;

  // 构造函数：接管协程句柄
  explicit Generator(handle_type h) : h_(h) {}

  // 析构函数：由于协程是在堆上分配的，必须手动 destroy 防止内存泄漏！
  ~Generator() {
    if (h_) h_.destroy();
  }

  // 禁用拷贝语义（句柄的所有权必须唯一）
  Generator(const Generator&) = delete;
  Generator& operator=(const Generator&) = delete;

  // 启用移动语义
  Generator(Generator&& oth) noexcept : h_(oth.h_) { oth.h_ = nullptr; }
  Generator& operator=(Generator&& oth) noexcept {
    if (this != &oth) {
      if (h_) h_.destroy();
      h_ = oth.h_;
      oth.h_ = nullptr;
    }
    return *this;
  }

  // ==========================================
  // 3. 用户接口
  // ==========================================
  // 驱动协程往前跑一步
  bool move_next() {
    if (h_) {
      h_.resume();  // 唤醒协程，直到它再次 co_yield 或自然结束
      if (h_.promise().exception_) {
        std::rethrow_exception(h_.promise().exception_);
      }
      return !h_.done();  // 如果协程没结束，返回 true
    }
    return false;
  }

  // 获取当前协程吐出来的数据
  T current_value() const { return h_.promise().value_; }
};

// ==========================================
// 🚀 业务逻辑：纯粹、无回调、无状态变量污染的 Generator
// ==========================================
Generator<int> fibonacci(int max_count) {
  int a = 0, b = 1;
  for (int i = 0; i < max_count; ++i) {
    // 每次执行到这里：
    // 1. 把 a 交给 promise.value_
    // 2. 冻结当前函数的局部变量 (a, b, i)
    // 3. 切回主线程 (main)
    co_yield a;

    int next = a + b;
    a = b;
    b = next;
  }
  // 执行到这里，隐式调用 co_return; -> 触发 return_void();
}

int main() {
  std::cout << "协程开始创建 (此时不会执行内部逻辑)...\n";
  auto gen = fibonacci(10);  // 触发 initial_suspend，瞬间挂起

  std::cout << "开始拉取数据:\n";
  // 外层的主线程，通过 move_next() 不断“榨取”协程
  while (gen.move_next()) {
    std::cout << gen.current_value() << " ";
  }
  std::cout << "\n生成完毕。\n";

  return 0;  // gen 离开作用域，触发析构函数，底层协程帧 (Coroutine Frame)
             // 被销毁
}