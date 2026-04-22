# QWEN.md — 可扩展 C++ RPC 框架

## 项目概览

这是一个基于 **C++20 + Protobuf + Linux TCP Socket** 的最小可运行 RPC 框架。架构采用 **Acceptor + WorkerLoop + Connection** 分层设计，服务端使用 one-loop-per-thread 模型（每个 worker 独立线程 + epoll + 协程），客户端提供同步 `Call`、异步 `CallAsync`（future-like）和协程 `CallCo`（direct coroutine waiter）三种调用方式。

### 核心技术栈

| 类别   | 技术                                          |
| ------ | --------------------------------------------- |
| 语言   | C++20（coroutine、concepts、source_location） |
| 构建   | CMake 3.16+                                   |
| 序列化 | Protobuf 3                                    |
| 网络   | Linux TCP Socket + epoll（非阻塞 I/O）        |
| 并发   | std::thread、std::atomic、协程                |
| 测试   | CTest（fork 隔离的集成测试模式）              |

### 架构分层

```
┌─────────────────────────────────────────────────┐
│                    业务层                        │
│  src/demo/  — CalcService.Add 示例              │
├─────────────────────────────────────────────────┤
│                    框架层                        │
│  src/server/  — RpcServer / WorkerLoop /        │
│               Connection / ServiceRegistry      │
│  src/client/  — RpcClient / EventLoop /         │
│               PendingCalls                      │
│  src/coroutine/ — Task<T> 协程基础              │
├─────────────────────────────────────────────────┤
│                    协议层                        │
│  src/protocol/ — Codec（长度前缀帧编解码）       │
├─────────────────────────────────────────────────┤
│                    公共层                        │
│  src/common/ — UniqueFd / RpcError / Log /      │
│                ThreadPool                        │
├─────────────────────────────────────────────────┤
│                    序列化层                      │
│  proto/ — rpc.proto / calc.proto / user.proto   │
└─────────────────────────────────────────────────┘
```

### 网络协议

自定义长度前缀帧协议：`[4字节大端长度][Protobuf序列化数据]`

---

## 构建与运行

### 依赖

```bash
sudo apt install -y cmake g++ protobuf-compiler libprotobuf-dev
```

### 构建

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -j$(nproc)
```

### 运行 Demo

```bash
# 终端1：启动服务端
./build/rpc_server_demo

# 终端2：启动客户端
./build/rpc_client_demo        # 同步 Call
./build/rpc_coroutine_client_demo  # 协程 CallCo
```

### 运行测试

```bash
cd build
ctest --output-on-failure              # 全部 22 个测试
ctest --output-on-failure -R 'call_co' # 仅 coroutine 测试
ctest --output-on-failure -R heartbeat # 仅心跳测试
```

### 运行 Benchmark

```bash
./build/rpc_benchmark 1000                    # 单连接延迟/吞吐
./build/rpc_thread_pool_benchmark 128 16 20  # inline vs thread_pool 对比
```

---

## 目录结构

```
rpc_project/
├── proto/                    # Protobuf 定义
│   ├── rpc.proto             # RpcRequest / RpcResponse / ErrorCode
│   ├── calc.proto            # CalcService.Add 示例
│   └── user.proto            # 用户相关业务 proto
├── src/
│   ├── client/               # 客户端实现
│   │   ├── rpc_client.h/cpp  # RpcClient 主类（含心跳机制）
│   │   ├── event_loop.h/cpp  # 轻量 epoll event loop
│   │   └── pending_calls.h/cpp  # request_id → result slot 管理器
│   ├── server/               # 服务端实现
│   │   ├── rpc_server.h/cpp  # Acceptor + 生命周期管理
│   │   ├── worker_loop.h/cpp # one-loop-per-thread worker
│   │   ├── connection.h/cpp  # 单连接状态机 + 帧解析 + 心跳 fast path
│   │   └── service_registry.h/cpp  # 服务方法注册表
│   ├── protocol/             # 协议层
│   │   └── codec.h/cpp       # 长度前缀帧编解码（ReadN/WriteN/ReadMessage/WriteMessage）
│   ├── common/               # 公共基础设施
│   │   ├── unique_fd.h       # fd RAII 包装
│   │   ├── rpc_error.h       # 统一错误码 + std::error_code
│   │   ├── log.h             # 结构化日志（source_location）
│   │   └── thread_pool.h/cpp # 固定线程池
│   ├── coroutine/            # 协程基础
│   │   └── task.h            # Task<T> 协程返回类型
│   └── demo/                 # 可运行示例
│       ├── server_main.cpp
│       ├── client_main.cpp
│       └── coroutine_client_main.cpp
├── tests/                    # 22 个测试文件（fork 隔离模式）
├── benchmarks/               # 性能基准测试
├── docs/                     # 设计文档
│   └── heartbeat_design.md   # 心跳机制设计文档
├── CMakeLists.txt
├── README.md
└── .gitignore
```

---

## 开发约定

### 编码风格

- **C++20** 标准，禁用 GNU 扩展（`CMAKE_CXX_EXTENSIONS OFF`）
- 头文件使用 `#pragma once`
- 类/方法注释使用 Doxygen 风格
- 命名：`snake_case` 函数/变量，`PascalCase` 类/结构体，`kPascalCase` 常量
- RAII 管理资源（`UniqueFd`、`std::unique_ptr`）

### 测试模式

- 使用 `fork()` 隔离服务端/客户端，避免端口冲突
- 子进程运行 mock server，父进程作为客户端验证
- `WaitServerReady()` 轮询 TCP connect 确认服务端就绪
- 测试无外部依赖，纯 assert + 退出码验证

### 线程模型

- **服务端**：Acceptor 线程（listen/accept） + N 个 WorkerLoop 线程（epoll + 协程） + M 个业务线程池（handler 执行）
- **客户端**：调用线程（发送请求） + Dispatcher 线程（epoll 接收响应）
- 连接归属单一 worker，禁止跨线程访问 Connection 对象

### 错误处理

- 业务层抛出 `RpcError`（继承 `std::exception`）
- 框架层统一收敛为 `Status(std::error_code, message)`
- 网络层使用 `rpc.proto` 的 `ErrorCode` 保证协议稳定

---

## 关键设计决策

### 服务端架构

| 组件         | 职责                                         |
| ------------ | -------------------------------------------- |
| `RpcServer`  | Acceptor：listen/accept/round-robin 分发连接 |
| `WorkerLoop` | 连接驱动：epoll + 协程 + 线程池回投          |
| `Connection` | 连接级协议：帧解析、读写缓冲、状态机、超时   |

### 客户端调用方式

| API           | 返回类型                     | 使用场景           |
| ------------- | ---------------------------- | ------------------ |
| `Call()`      | `RpcCallResult`              | 同步阻塞调用       |
| `CallAsync()` | `std::future<RpcCallResult>` | 异步 future 调用   |
| `CallCo()`    | `Task<RpcCallResult>`        | 协程 co_await 调用 |

### 心跳机制

客户端通过 `RpcClientOptions.heartbeat_interval/timeout` 配置：
- 默认 30s 发送心跳，45s 超时关闭
- 复用 `RpcRequest` 协议，保留服务名 `"__Heartbeat__"`
- 服务端快速路径识别，不进线程池、不查注册表，直接返回空 Response
- 心跳响应通过 `hb_` 前缀识别，跳过 `pending_calls_->Complete()`

详见 [docs/heartbeat_design.md](docs/heartbeat_design.md)。
