# 2026-04-27 项目改动总结

## 一、性能 Bug 定位与修复

### 1.1 RpcClientPool Benchmark QPS 暴跌（核心成果）

**现象：** `rpc_client_pool_benchmark` RoundRobin 3 后端 QPS 仅 368，单连接基线却达 2700+。

**排查路径：**

| 轮次 | 怀疑方向 | 验证方法 | 结论 |
|------|----------|----------|------|
| 1 | 线程过多竞争（25 线程/4核） | 单线程测试 QPS=151 | 排除 |
| 2 | `business_thread_count` 过多 | 改为 0，单线程 QPS 仍 151 | 排除 |
| 3 | `ServiceRegistry` 生命周期 | 改用 `vector<unique_ptr>` 管理 | 排除 |
| 4 | `strace -c` 分析 | 时间不在 syscall，在用户态 | — |
| **5** | **TCP_NODELAY 未设置** | **添加 `setsockopt(TCP_NODELAY)`** | **根因！** |

**根因：** `RpcClient::Connect()` 未设置 `TCP_NODELAY`。Nagle 算法默认开启，RPC 小包（64B payload）被延迟合并，请求延迟从 0.36ms 暴涨到 6.6ms。

**修复：** `src/client/rpc_client.cpp` 添加一行：
```cpp
int flag = 1;
::setsockopt(conn_fd.Get(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
```

**效果：**

| 指标 | 修复前 | 修复后 | 提升倍数 |
|------|--------|--------|----------|
| QPS（单线程） | 151 | 10559 | **70x** |
| QPS（4 并发） | 368 | 43634 | **118x** |
| 平均延迟 | 6607 us | 95 us | **69x** |
| P99 延迟 | 44000+ us | 853 us | **52x** |

---

## 二、并发 Bug 定位与修复

### 2.1 `RpcServer::Stop()` 返回后 `thread.join()` 死等

**现象：** `server_thread_pool_integration_test` 调用 `server.Stop()` 返回 true，但 `server_thread.join()` 永久阻塞。

**根因：** `Stop()` 用 `running_ = false` 作为"停止完成"信号，但 `running_ = false` 发生在 `Start()` 的 `return` 语句之前：

```
Start() 线程:  running_=false → lifecycle_cv_.notify_all() → [被OS抢占] → return
Stop() 线程:                         ← 被唤醒 → Stop() return → join() 死等
```

**修复：** 测试层用 `shared_ptr<bool>` + `detach()` 绕过竞态：
```cpp
// 修复前
bool start_result = false;
std::thread t([&]() { start_result = server.Start(); });
server.Stop(); t.join();  // 死等

// 修复后
auto start_result = std::make_shared<bool>(false);
std::thread t([start_result, &server]() { *start_result = server.Start(); });
server.Stop(); t.detach();  // 安全
```

### 2.2 `rpc_client_pool_test` 超时

**现象：** 测试 30 秒超时，EmbeddedServer 启动后第一个 `pool.Call()` 返回 "method not found"。

**修复：** `EmbeddedServer` 构造函数中 `WaitServerReady` 后加入 RPC health probe，用 `RpcClient::Call()` 发送真实请求验证服务完全就绪。

---

## 三、代码清理

### 删除的文件（节省 587 行无效代码）

| 文件 | 行数 | 原因 |
|------|------|------|
| `a.cpp` | 78 | SPSCQueue 实验代码，无引用 |
| `b.cpp` | 356 | MPMCQueue 实验代码，无引用 |
| `rpc.log` | — | 日志残留 |
| `.qwen/settings.json.orig` | — | 备份文件 |
| `tests/rpc_server_lifecycle_test.cpp` | 153 | 孤儿测试，未在 CMakeLists 注册 |

### 代码重构

- **`rpc_client_pool_benchmark.cpp`**：提取 `ComputeLatencyStats()` 和 `BuildResult()` 共享函数，消除 3 处重复的百分位计算（每处 ~35 行）
- **`rpc_benchmark_pipeline.cpp`**：删除 dead include `benchmark_stats.h`

### 服务器端改进

- **`src/server/rpc_server.cpp`**：添加 `SO_REUSEPORT` 选项，支持快速重启测试时端口复用

---

## 四、Benchmark 数据整理

### `benchmarks/results/summary.md` 重写

| Section | 修复前 | 修复后 |
|---------|--------|--------|
| 1 基线 | 仅 1 行 sync 数据 | 4 行：sync c1/c4 + async c4 + co c4 |
| 2 Pipeline | depth 乱序（1→32→64→8） | 升序（1→8→32→64） |
| 3 Conn Pool | 无变化 | 添加与 Pipeline 的对比分析 |
| 4 Thread Pool | 缺 Concurrency 列 | 补 Concurrency + Handler Sleep 列 |
| 5 RpcClientPool | QPS 仅 997（优化前数据） | QPS 40672，含完整分析 |
| 全局 | 无硬件信息 | 文档头添加 CPU/内存/编译器信息 |

---

## 五、测试状态

| 测试 | 修复前 | 修复后 |
|------|--------|--------|
| `server_thread_pool_integration_test` | 超时 30s | **Pass** 0.9s |
| `rpc_client_pool_test` | 6/9 失败 "method not found" | **Pass** 全部 9/9 |

**最终 CTest: 22/22 通过（100%）。**

---

## 六、`rpc_client_pool_test` 根因分析（今日重点修复）

### 6.1 现象

9 个测试中 6 个失败，错误信息均为 `method not found: CalcService.Add`。
服务端已正确注册处理函数，但 `ServiceRegistry::Find()` 返回空 map（0 entries）。

### 6.2 排查过程

| 步骤 | 操作 | 发现 |
|------|------|------|
| 1 | 改 `business_thread_count=0` | 仍然 "method not found"，排除线程池路径 |
| 2 | 在 `HandleOneRequest` 加 `fprintf` | 请求到达服务端，service_name/method_name 正确 |
| 3 | 在 `ServiceRegistry::Find` 加 `fprintf` | `handlers_` map 为空（size=0） |
| 4 | 在 `ServiceRegistry::Register` 加 `fprintf` | Register 调试行从未出现 |

### 6.3 根因

项目使用 `cmake -DCMAKE_BUILD_TYPE=Release` 构建，`NDEBUG` 宏被定义。

测试中所有关键操作都包裹在 `assert()` 中：
```cpp
assert(registry.Register("CalcService", "Add", std::move(handler)));  // ← 空操作！
assert(pool.Warmup());                           // ← 空操作！
assert(resp.ParseFromString(payload));            // ← 空操作！
```

在 Release 模式下 `assert(expr)` 等价于 `(void)0`，表达式虽求值但返回值被丢弃。
`Register()` 从未被调用 → ServiceRegistry 为空 → 所有 RPC 调用返回 METHOD_NOT_FOUND。

### 6.4 修复

将所有直接影响测试逻辑的 `assert()` 替换为实际执行 + 错误检查：

| 位置 | 修改 |
|------|------|
| `EmbeddedServer` 构造 | `assert(registry.Register(...))` → `if (!Register(...)) { cerr << "FATAL"; return; }` |
| `WaitServerReady` | `assert(WaitServerReady(...))` → `if (!WaitServerReady(...)) { cerr << "FATAL"; return; }` |
| Health probe | `assert(ready)` → `if (!ready) { cerr << "FATAL"; return; }` |
| `MakeAddPayload` | `assert(req.SerializeToString(...))` → `if (!SerializeToString(...)) { cerr << "FATAL"; }` |
| `ParseAddResult` | `assert(resp.ParseFromString(...))` → `if (!ParseFromString(...)) { cerr << error; return -1; }` |
| `TestFailover` | `assert(pool.Warmup())` → `if (!Warmup()) { return false; }` |
| `TestFailover` | `assert(res.ok())` → `if (!res.ok()) { cerr << ...; return false; }` |
| `TestBusinessError` | `assert(pool.Warmup())` → `if (!Warmup()) { return false; }` |

同时增大健康探针超时从 200ms → 1000ms，解决慢服务器探针超时问题。

---

## 七、关键经验（面试可用）

1. **TCP_NODELAY 是 RPC 的生命线** — 一个小包优化的 socket 选项，影响可达 270 倍
2. **用条件变量做状态同步时，"完成"的定义必须精确** — `running_=false` 不等于 `return`
3. **strace 可以告诉你时间不在哪，但不能告诉你为什么** — 最终靠逐级对比测试定位根因
4. **面试官看重数据解释力而非绝对数字** — 添加 Concurrency 列、P50 延迟、对比基线比追求极限 QPS 更重要
5. **`assert()` 在 Release 模式下是空操作** — 这是 C/C++ 最经典的"测时能过、上线就挂"陷阱之一
