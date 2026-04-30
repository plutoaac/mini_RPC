# 踩坑记录与面试考点

项目期间遇到并修复的所有关键 bug、设计陷阱、架构决策，按面试可讲的逻辑组织。

---

## 目录

1. [Bug 猎手：定位与修复](#1-bug-猎手)
2. [竞态条件与并发陷阱](#2-竞态条件)
3. [设计陷阱与 API 误用](#3-设计陷阱)
4. [架构决策与权衡](#4-架构决策)
5. [ThreadPool 优化全记录](#5-threadpool-优化)
6. [面试一句话总结](#6-面试一句话总结)

---

## 1. Bug 猎手

### 1.1 TCP_NODELAY — 270 倍 QPS 的 socket 选项

**现象**：`rpc_client_pool_benchmark` QPS 仅 151，预期 10k+。

**排查路径**：

| 步骤 | 猜想 | 实验 | 结论 |
|------|------|------|------|
| 1 | 线程过多竞争 | 单线程测 QPS=151 | 不是线程问题 |
| 2 | `business_thread_count` 影响 | 设为 0，QPS 仍 151 | 不是线程池问题 |
| 3 | `ServiceRegistry` 生命周期 | 改用 unique_ptr 管理 | 不是 |
| 4 | `strace -c` 看时间分布 | 大部分在用户态 | 排除 syscall 瓶颈 |
| **5** | **Nagle 算法** | **加 `setsockopt(TCP_NODELAY)`** | **根因！** |

**根因**：`RpcClient::Connect()` 未设置 `TCP_NODELAY`。Nagle 算法默认启用，64B payload 被延迟合并发送，延迟从 0.36ms 暴涨到 6.6ms。

**修复**：`src/client/rpc_client.cpp` 加一行：
```cpp
int flag = 1;
setsockopt(conn_fd.Get(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
```

| 指标 | 修复前 | 修复后 | 提升 |
|------|--------|--------|------|
| QPS（单线程） | 151 | 10,559 | **70x** |
| QPS（4 并发） | 368 | 43,634 | **118x** |
| P99 延迟 | 44ms+ | 853us | **52x** |

**面试回答模板**：

> "RPC 场景最经典的性能陷阱之一。64B 小包被 Nagle 延迟合并，TCP 等不到下一个包就只能超时发送。一句话 `setsockopt(TCP_NODELAY)` 修了 270 倍 QPS 差距。排查时先逐级排除线程池、服务注册、syscall 开销，最后用二分法确认是 TCP 层问题。"

---

### 1.2 Coroutine Double Free — 协程生命周期竞态

**现象**：压测时 `double free detected in tcache 2`，ASan 栈指向 `PendingCalls::Complete() → coroutine_handle.resume()`。

**根因**：完成信号（`set_value`）过早发布——在 `return_value()` 中直接 `set_value`，外部 `Task` 析构看到 future ready 就 `destroy()` 协程帧。但协程此时还在 `final_suspend` 路径，发生并发销毁。

**修复**（`src/coroutine/task.h`）：

1. `return_value()` 只暂存结果，不发布 future
2. 新增 `PublishCompletion()`，在 `final_suspend` 统一发布
3. `final_suspend` 使用对称转移（返回 continuation 句柄，不嵌套 `resume()`）

**时序对比**：

| 旧（有 bug） | 新（修复后） |
|-------------|-------------|
| `co_return` → `return_value()` 立即 `set_value` | `co_return` → `return_value()` 只缓存 |
| 外部 `Task` 看到 ready → `destroy()` | 进入 `final_suspend` |
| 协程还在尾部 | `PublishCompletion()` 发布 future |
| **double free** | 外部安全 `destroy()` |

**面试回答模板**：

> "协程的完成信号必须晚于真实生命周期终点。我的修复是把 `set_value` 从 `return_value` 挪到 `final_suspend`，并用对称转移避免嵌套 `resume()`。本质是生产者-消费者同步问题：消费者不能比生产者先认为工作完成。"

---

### 1.3 Server Connection Hang — 阻塞 fd 遇上非阻塞事件循环

**现象**：`server_connection_test` 调用 `Connection::Serve()` 后 `join()` 永久阻塞。

**根因**：测试用 `socketpair()` 创建 fd，交给 `Serve()`。`Serve()` 的读循环按非阻塞 socket 设计——读到 `EAGAIN` 返回。但 socketpair 默认是阻塞模式，`recv()` 数据读完后不返回 `EAGAIN` 而是一直阻塞等新数据，线程卡在 `recv()` 内部。

**修复**：`Connection::Serve()` 启动时对 fd 执行 `fcntl(O_NONBLOCK)`，统一读写语义。

**面试回答模板**：

> "事件循环 + 阻塞 fd 是经典的模式不匹配 bug。`recv()` 在非空数据读完后，阻塞 fd 会继续等待而不是返回 EAGAIN。修复很简单——启动时统一设 `O_NONBLOCK`。排查这类问题关键是看 `strace`：如果卡在 `recv` 而不是 `epoll_wait`，就是 fd 模式问题。"

---

### 1.4 LeastInflight Segfault — stack-use-after-scope

**ASan 报告**：`AddressSanitizer: stack-use-after-scope` 在 `rpc_client_pool_benchmark.cpp:443`

**代码**：
```cpp
for (int i = 0; i < concurrency; ++i) {
    const int req_count = base + (i < rem ? 1 : 0);
    workers.emplace_back([&, i]() {   // [&] 捕获了 req_count 的引用！
        for (int j = 0; j < req_count; ++j) { ... }
    });
}
```

`req_count` 是循环内局部变量，lambda `[&]` 引用捕获。循环迭代到下一轮时变量析构，已启动的线程仍在读它 → use-after-scope。

**修复**：`[&, i, req_count]` 按值捕获。

**面试回答模板**：

> "C++ lambda 最常见的 bug 之一——引用捕获了一个循环内的栈变量。循环结束、线程还在跑、栈帧已回收。ASan 直接指到行号。修复是一字之差：`[&, i]` → `[&, i, req_count]`。"

---

### 1.5 TOCTOU Div-by-Zero in LeastInflightBalancer

**ASan 报告**：`FPE (floating point exception) at load_balancer.cpp:40`

```cpp
// 步骤 1：扫描最小 inflight
min_inflight = candidates[0]->GetInflightCount();  // = 5
// ... 步骤 2：收集 tie_indices ...
// 步骤 1 和 2 之间，另一个线程完成了请求，该候选 inflight 变成 4
// 步骤 2 发现没有候选 inflight == 5，tie_indices 为空
return tie_breaker_++ % tie_indices.size();  // div by zero
```

**修复**：tie_indices 为空时重新扫描（第三次），保证至少一个候选。两次 `GetInflightCount()` 之间丢失 `min_inflight` 匹配的候选是 TOCTOU 的正常表现，不能假设步骤 1 和步骤 2 看到的状态一致。

**面试回答模板**：

> "经典 TOCTOU——两次调用 `GetInflightCount()` 之间状态变了。修复不是加锁（会回到全局锁），而是兜底：tie_indices 为空就重新扫描一遍。对负载均衡来说，偶尔的多扫一遍比加锁的代价小得多。"

---

## 2. 竞态条件

### 2.1 Stop/Join 竞态 — `running_=false` ≠ `return`

**现象**：`server.Stop()` 返回 true，但 `server_thread.join()` 永久阻塞。

**根因**：`Stop()` 用 `running_ = false` 作为"完成"信号，`Start()` 中设置 `running_ = false` 后还有清理代码才执行到 `return`。OS 可能在 `running_=false` 之后、`return` 之前调度走 `Start()` 线程。

```
Start() 线程:  running_=false → cv.notify() → [被OS抢占] → return
Stop() 线程:                         ← 被唤醒 → Stop() return → join死等
```

**修复**：测试层用 `shared_ptr<bool>` + `detach()` 绕过。后续改为：`Stop()` 用 `lifecycle_cv_.wait(lock, []{ return !running_; })` 真正等 `Start()` 返回后才返回。

**面试回答模板**：

> "条件变量同步时，'完成'的定义必须精确到最后一个副作用。`running_ = false` 不等于 `return`——中间还有 `CloseAcceptor`、`StopWorkers` 等清理。修复是让 `Stop()` 用 `cv.wait` 而不是立即返回。"

---

### 2.2 RpcClientPool::CallAsync — 线程生命周期

**现象**：`CallAsync` 后台线程捕获裸 `this`，pool 析构后 use-after-free。

**修复**：lambda 自包含——不捕获 `this`，直接操作 Channel 的 atomic 成员。`RecordCallResult` 和 `RecordReachability` 的内联逻辑直接写在 lambda 里。

```cpp
std::thread([ch, inner = std::move(inner_future), promise, max_failures]() mutable {
    auto result = inner.get();
    // 直接操作 ch->success_count, ch->fail_count, ch->healthy 等 atomic 成员
}).detach();
```

**面试回答模板**：

> "`std::thread(pool, [this]{...}).detach()` 是典型的生命周期 bug。修复是不让 lambda 需要 `this`——把需要的 channel 统计逻辑内联到 lambda，只捕获 channel 指针和配置。在 benchmark 中 pool 覆盖 future 生命周期，所以安全。"

---

### 2.3 assert(server.Stop()) — NDEBUG 下的隐藏空操作

**现象**：Release 模式下 `server_thread_pool_integration_test` 超时 120s。

**代码**：
```cpp
assert(server.Stop());  // Release 下 = (void)0，Stop() 从未调用！
server_thread.join();    // 永久阻塞，因为 Start() 还在 accept loop
```

**修复**：
```cpp
server.Stop();    // 直接调用
assert(server_thread.joinable());
server_thread.join();
assert(start_result);
```

**面试回答模板**：

> "`assert()` 在 Release（NDEBUG）下是不求值的空操作。`assert(func())` 在 Debug 能过测，Release 下 `func()` 从未被调用。这是 C/C++ 最经典的'测时能过、上线就挂'陷阱。"

---

## 3. 设计陷阱

### 3.1 assert(registry.Register(...)) — Release 下服务未注册

**现象**：Release 模式下 6/9 个 `rpc_client_pool_test` 返回 "method not found"。

**根因**：`assert(registry.Register(...))` 在 NDEBUG 下不执行注册逻辑。`ServiceRegistry` 为空。

**修复**：所有 benchmark/test 中用 `assert` 包裹的有副作用表达式改为显式检查：
```cpp
if (!registry.Register("Service", "Method", handler)) {
    std::cerr << "FATAL: register failed\n";
    std::abort();
}
```

这是贯穿整个项目的修复——`rpc_thread_pool_benchmark.cpp`、`rpc_client_pool_test.cpp`、`server_thread_pool_integration_test.cpp` 等多处。

---

### 3.2 std::function<void()> 是 noexcept movable 的

**误解**：认为 `std::function<void()>` 不是 `nothrow_move_constructible`，需要用 `unique_ptr` 包装后再放入 MpscRingQueue。

**验证**：
```cpp
// GCC 13.1.0 实测
std::is_nothrow_move_constructible_v<std::function<void()>> == true
std::is_nothrow_move_assignable_v<std::function<void()>> == true
```

**优化结果**：去掉 `unique_ptr` 包装后，任务对象直接存储在 MpscRingQueue 的 64 字节对齐 Slot 内，每任务省 1 次堆分配。

---

### 3.3 std::launch::deferred 不是真异步

**现象**：`RpcClientPool::CallAsync` 返回的 future 在 `wait_for()` 时返回 `std::future_status::deferred`。

**根因**：`std::async(std::launch::deferred, lambda)` 不会启动新线程，lambda 在调用 `.get()` 时同步执行。

**修复**：显式 `std::thread(lambda).detach()` + `std::promise/std::shared_future` 实现真异步。后台线程等 inner future 完成后设 promise，统计在设值前记录。

---

### 3.4 Bounded Queue 的 QPS"下降"不是退化

在 ThreadPool microbenchmark 中，新版 QPS 在某些场景低于旧版（如 2w×2p, 4w×4p）。

**原因**：新版 MpscRingQueue 定容 1024/worker，满时生产者阻塞（`cv.wait`）。QPS = total_tasks / wall_clock，包含生产者等待时间。旧版无界 deque 从不阻塞，QPS 自然更高——但内存无上限。

**结论**：延迟指标（p50/p95）更真实反映调度性能。新版 p50 普遍改善 5x~779x。QPS"下降"是有界队列背压的设计选择，不是 bug。

**面试回答模板**：

> "Bounded queue 的 QPS 低是因为生产者等待时间计入了总耗时——这是 backpressure 的设计代价。旧版无界队列 QPS 高但有无限内存风险。判断性能要看 p50/p95 延迟，不是包含等待时间的 QPS。"

---

## 4. 架构决策

### 4.1 one-loop-per-thread + 协程

- Acceptor 线程只 accept 和分发，不进业务
- 每个 Worker 独立 epoll + eventfd
- 每连接一个协程，挂起不占线程
- 业务 ThreadPool 与 I/O Worker 解耦

```text
Acceptor → [lock + queue + eventfd] → Worker(epoll + coroutine + connections)
                                              ↓
                                        ThreadPool (慢 handler)
```

---

### 4.2 跨线程通信：queue + eventfd 而非共享 epoll

**错误做法**：Acceptor 线程直接 `epoll_ctl(ADD)` 操作 Worker 的 epoll_fd — 多线程操作同一个 epoll 导致 data race。

**正确做法**：Acceptor → mutex 锁队列 → push fd → write eventfd(1) 唤醒 → Worker epoll_wait 返回 → 从队列取 fd → Worker 自己 epoll_ctl(ADD)。

---

### 4.3 MpscRingQueue 核心设计

- 环形缓冲区，capacity 为 2 的幂，`pos & mask` 取模
- 三态 sequence：空闲 → 已发布 → 已消费，环绕复用
- 多生产者 CAS 竞争入队，单消费者无锁出队
- `alignas(64)` 消除伪共享
- placement-new / destroy_at 管理对象生命周期

**面试回答模板**：

> "实现了一个无锁 MPSC 环形队列。核心是用 sequence 三态机控制槽位复用，生产者 CAS 抢槽位后 placement-new 写入、release 发布，消费者 acquire 读取后 destroy_at 释放、release 归还。性能提升关键是从全局 mutex 变成 per-worker 的无锁队列。"

---

## 5. ThreadPool 优化

### 优化清单

| # | 优化 | 效果 |
|---|------|------|
| 1 | 零堆分配（Task 直接存 Slot） | 每任务省 1 次 new/delete |
| 2 | `is_waiting` — 只睡时 notify | 消除 90%+ 无效 futex |
| 3 | `PopBatch` 批量出队 | 函数调用降 32x |
| 4 | 批量原子更新 | 原子写降 32x |
| 5 | `thread_local` round-robin | Submit 零竞争 |
| 6 | `alignas(64)` 隔离 | 消除伪共享 |
| 7 | 背压阻塞（cv.wait） | 零任务丢失 |
| 8 | `pending` 原子计数 | 消除 data race |
| 9 | `active_workers` ±1 | 语义修正 |

### Benchmark 关键数据（Release -O3, 4-core Xeon）

| 场景 | OLD p50 | NEW p50 | 改善 |
|------|---------|---------|------|
| 空任务 4w×4p | 288us | 17us | 17x |
| 10us busy-wait 4w×4p | 29ms | 11ms | 2.8x |
| 高并发 4w×32p | 16ms | 37us | 434x |
| 1 worker | 30us | 6us | 5x |

---

## 6. 面试一句话总结

> "在这个项目里我处理了 RPC 的性能瓶颈（TCP_NODELAY 导致 270x QPS 差距）、协程生命周期竞态（double free）、多个线程安全 bug（TOCTOU 除零、lambda 悬垂捕获、Stop/Join 竞态）、Release 下的 assert 陷阱，以及自己实现了一个无锁 MPSC 队列并集成进线程池（p50 改善最高 434x）。每个 bug 我都写了复盘文档，记录了排查路径和根因分析。"

---

*关联文档*：`docs/coroutine_double_free_bug.md`、`docs/server_connection_hang_debug.md`、`docs/today_changes.md`、`benchmarks/results/benchmark_analysis.md`
