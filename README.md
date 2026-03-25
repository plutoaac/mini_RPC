# 可扩展 C++ RPC 框架（最小可运行版本）

本项目实现了一个基于 **C++20 + Protobuf + Linux TCP Socket** 的最小可运行 RPC 框架。

当前版本目标是“闭环可运行 + 分层清晰 + 便于演进”，后续可以平滑升级到：
- C++20 coroutine 版本
- io_uring 异步 IO 版本

本阶段已从纯阻塞式客户端分发循环，演进到轻量 event loop / reactor 雏形。

当前版本已在现有 async client 底层之上新增最小 coroutine API（`CallCo` + `Task<T>`），
用于提供 `co_await` 编程体验，不改变现有 I/O 与分发模型。

## 1. 项目介绍

框架能力（当前版本）：
- 通用 RPC 请求/响应消息（`RpcRequest` / `RpcResponse`）
- 自定义协议：`[4字节长度][protobuf数据]`
- 阻塞式 TCP 通信
- 服务端方法注册与分发（`service_name + method_name -> handler`）
- 客户端通用调用接口
- 客户端异步调用接口（`CallAsync`，返回 `std::future<RpcCallResult>`）
- 客户端协程调用接口（`CallCo`，返回 `Task<RpcCallResult>`）
- 基于 `std::error_code` 的统一错误体系
- 基于 RAII 的 socket fd 生命周期管理（`UniqueFd`）
- 基于 `std::source_location` 的轻量结构化日志
- 客户端基础超时能力（`SO_SNDTIMEO` / `SO_RCVTIMEO`）
- 客户端读写分工模型（调用线程发送 + event loop 驱动可读事件）
- 自动化测试入口（协议层、注册中心、端到端）
- Benchmark 入口（单连接延迟与吞吐）
- PendingCalls（`request_id -> result slot`）与连接关闭竞态保护

## 2. 架构说明

分层如下：
- 协议层（`src/protocol`）：负责网络帧编解码，不关心业务类型
- 序列化层（`proto`）：由 protobuf 定义通用 RPC 消息和业务消息
- 公共基础层（`src/common`）：错误体系、fd RAII、日志设施
- 框架层（`src/server`、`src/client`）：负责注册、分发、调用、错误处理
- 业务层（`src/demo`）：`CalcService.Add` 示例

框架层与业务层解耦：
- 框架层只传递 `bytes payload`
- 业务 protobuf 的解析与序列化在业务 handler 内完成

## 3. 模块说明

- `proto/rpc.proto`
  - `RpcRequest`: `request_id`、`service_name`、`method_name`、`payload`
  - `RpcResponse`: `request_id`、`error_code`、`error_msg`、`payload`
  - `ErrorCode`: `OK`、`METHOD_NOT_FOUND`、`PARSE_ERROR`、`INTERNAL_ERROR`

- `proto/calc.proto`
  - `AddRequest { int32 a, int32 b }`
  - `AddResponse { int32 result }`

- `src/protocol/codec.*`
  - 长度前缀协议编解码
  - 阻塞式读写（完整读 N 字节 / 完整写 N 字节）

- `src/server/service_registry.*`
  - 通用注册表
  - handler 接口：输入请求 `bytes`，输出响应 `bytes`
  - `Find()` 返回引用包装，避免复制 `std::function`

- `src/server/rpc_server.*`
  - TCP 监听/接收连接（全链路 `UniqueFd` 管理）
  - 解码请求、查找 handler、执行并返回响应
  - 异常与错误统一映射到 `RpcResponse`

- `src/client/rpc_client.*`
  - 连接服务端
  - 构造并发送 `RpcRequest`
  - 独立 dispatcher 线程运行 event loop，监听 socket 可读事件
  - 非阻塞读取并解析长度前缀响应帧
  - 按 `request_id` 精确完成对应 in-flight 请求（含 async future）
  - 提供 `CallAsync`（future-like 最小版本）
  - 提供 `CallCo`（coroutine 最小桥接层）
  - 同步 `Call` 基于 `CallAsync().get()` 封装
  - 通过 `Status(std::error_code + message)` 返回统一错误
  - 通过 `PendingCalls` 进行请求与响应关联（支持多 in-flight）

- `src/client/event_loop.*`
  - 轻量 epoll event loop（单线程、单连接 fd）
  - 负责可读事件等待、超时 tick 与 wakeup
  - 作为后续 coroutine 客户端调度的基础骨架

- `src/client/pending_calls.*`
  - 维护 `request_id -> result slot` 的线程安全表
  - 支持 `Add / BindAsync / Complete / FailTimedOut / FailAll`
  - dispatcher 线程可直接完成 async promise，不需要每请求 watcher 线程
  - `FailAll` 仅标记未完成槽位，避免覆盖已完成结果（修复关闭连接竞态）

- `src/common/rpc_error.h`
  - 定义框架错误枚举与 `std::error_code` category
  - 提供 protobuf 错误码与框架错误码的双向转换

- `src/common/unique_fd.h`
  - 提供 move-only 的 fd RAII 包装，避免手动 `close` 泄漏

- `src/common/log.h`
  - 提供 `LogInfo/LogWarn/LogError`
  - 自动附带 `file:line:function`（`std::source_location`）

- `src/demo/server_main.cpp`
  - 注册 `CalcService.Add`

- `src/demo/client_main.cpp`
  - 调用 `Add(1,2)` 并输出结果

## 4. 构建方法

### 依赖

请先安装：
- `cmake`
- `g++`（支持 C++20）
- `protobuf` 与 `protoc`

示例（Debian/Ubuntu）：

```bash
sudo apt update
sudo apt install -y cmake g++ protobuf-compiler libprotobuf-dev
```

### 构建

```bash
cd rpc_project
cmake -S . -B build
cmake --build build -j
```

## 5. 运行 demo 步骤

先启动服务端：

```bash
./build/rpc_server_demo
```

另开一个终端启动客户端：

```bash
./build/rpc_client_demo
```

预期客户端输出：

```text
Add(1,2) = 3
```

## 6. RPC 调用流程说明

1. 客户端将业务请求（`calc::AddRequest`）序列化为 `bytes payload`
2. 客户端构造 `RpcRequest`：填入 `request_id/service_name/method_name/payload`
3. `Codec` 发送数据：`[4字节长度][protobuf序列化后的RpcRequest]`
4. 服务端读取并反序列化 `RpcRequest`
5. 服务端通过 `ServiceRegistry` 查找 `CalcService.Add` handler
6. handler 解析业务 payload，执行加法，返回响应 payload
7. 服务端构造 `RpcResponse`（含错误码/错误信息）并回写
8. 客户端读取 `RpcResponse`，成功时解析 `calc::AddResponse` 得到结果

## 7. 错误体系与日志

### 统一错误体系

- 业务层可抛出 `RpcException`
- 框架层统一收敛为 `Status`（包含 `std::error_code` 与 message）
- 网络层响应仍使用 `rpc.proto` 中的 `ErrorCode` 字段，保证协议稳定

### 日志

- 日志接口：`LogInfo/LogWarn/LogError`
- 每条日志自动输出来源位置：文件名、行号、函数名
- 适合后续替换为更完整的日志后端（如 spdlog / tracing）

## 8. 超时机制

客户端提供基础超时配置（阻塞式最小实现）：

- `send_timeout`：写请求超时
- `recv_timeout`：读响应超时

示例：

```cpp
rpc::client::RpcClient client(
    "127.0.0.1", 50051,
    {.send_timeout = std::chrono::milliseconds(1000),
     .recv_timeout = std::chrono::milliseconds(1000)});
```

说明：当前版本基于 `SO_SNDTIMEO` / `SO_RCVTIMEO`，后续可升级到更细粒度的 per-request deadline。

## 9. CallAsync（future-like 最小版本）

客户端已提供最小异步接口：

```cpp
std::future<RpcCallResult> CallAsync(
  std::string_view service_name,
  std::string_view method_name,
  std::string_view request_payload);
```

实现要点：

- 复用当前 `request_id + PendingCalls + dispatcher` 主链路
- `CallAsync` 负责发请求并返回 `future`
- `CallAsync` 在 PendingCalls 注册 async 等待状态（promise + deadline）
- dispatcher 的 event loop 收到可读事件后，解析响应并按 `request_id` 直接完成 promise
- 同步 `Call` 改为 `CallAsync(...).get()`，保持现有调用行为

定位说明：

- 这是一个“最小可工作”过渡版，目标是在保持结构简单的前提下引入 reactor 思路
- 当前结构已为后续 coroutine/awaitable 版本铺路（I/O readiness 与完成分发职责已分层）

## 10. CallCo（最小 coroutine 接口层）

客户端新增了 coroutine 友好接口：

```cpp
Task<RpcCallResult> CallCo(
  std::string_view service_name,
  std::string_view method_name,
  std::string_view request_payload);
```

最小使用示例：

```cpp
rpc::coroutine::Task<void> Demo(rpc::client::RpcClient& client,
                                const std::string& payload) {
  auto result = co_await client.CallCo("CalcService", "Add", payload);
  if (!result.ok()) {
    co_return;
  }
  co_return;
}
```

定位说明：

- coroutine 层是增量包装层，复用现有 `CallAsync + PendingCalls + dispatcher/event loop`
- 当前版本主要提升调用侧编程模型，不引入完整 coroutine runtime
- bridge 方式仍有额外 future/awaiter 开销，但实现简单且便于学习

可运行示例：

```bash
./build/rpc_coroutine_client_demo
```

## 11. 测试

本项目已提供 12 类可重复执行测试：

- `event_loop_test`：event loop 基础行为
  - 可读事件触发
  - 超时返回
  - wakeup 事件触发

- `codec_test`：协议层
  - 正常 encode/decode
  - 长度为 0 的帧报错
  - 超过最大帧长度报错
  - protobuf 解析失败报错

- `service_registry_test`：注册中心
  - 注册成功
  - 重复注册失败
  - 未注册方法返回空
  - 多线程并发注册/查找基础正确性

- `e2e_test`：端到端
  - Add(1,2)=3
  - 不存在方法返回 `METHOD_NOT_FOUND`

- `pending_calls_test`：pending 表
  - Add/Complete/Pop 基本行为
  - FailAll 行为
  - 并发 Add/Complete/Pop 基础正确性

- `out_of_order_dispatcher_test`：乱序响应分发
  - 服务端故意按请求接收顺序的逆序返回响应
  - 验证客户端 `CallAsync` 在乱序响应下通过 `request_id` 正确匹配结果
  - 覆盖“连接关闭 + 已完成结果”竞态场景

- `call_async_timeout_test`：异步超时隔离
  - 多个 `CallAsync` 并发请求中，单个慢请求超时返回
  - 验证其他请求仍可成功返回，不被超时请求污染

- `call_async_close_test`：关闭连接后的异步收敛
  - `Close()` 后未完成的 async future 必须收到失败结果
  - 验证不会出现 future 永远不完成

- `call_co_basic_test`：coroutine 调用基础成功
  - `co_await client.CallCo(...)` 可获得正确结果
  - 多个 coroutine 请求结果正确

- `call_co_out_of_order_test`：coroutine 乱序响应分发
  - 服务端逆序返回响应
  - 验证 coroutine 调用仍按 request_id 正确匹配

- `call_co_timeout_test`：coroutine 超时收敛
  - 并发 coroutine 请求中慢请求超时
  - 快请求结果不受影响

- `call_co_close_test`：coroutine 连接关闭收敛
  - `Close()` 后未完成 coroutine 调用返回失败结果

运行方式：

```bash
cd rpc_project
cmake -S . -B build
cmake --build build -j
cd build
ctest --output-on-failure
```

仅运行乱序测试：

```bash
cd rpc_project/build
ctest --output-on-failure -R 'out_of_order_dispatcher_test|call_async_timeout_test|call_async_close_test'
```

仅运行 coroutine 相关测试：

```bash
cd rpc_project/build
ctest --output-on-failure -R 'call_co_basic_test|call_co_out_of_order_test|call_co_timeout_test|call_co_close_test'
```

## 12. Benchmark

提供单连接基准程序 `rpc_benchmark`，默认执行 1000 次 Add 调用，输出：

- 平均延迟（avg）
- p95 延迟
- 吞吐（qps）

运行示例：

```bash
cd rpc_project/build
./rpc_benchmark 1000
```

## 13. 可扩展性说明

当前代码保持阻塞式最小闭环，但接口设计已为后续扩展预留：
- `Codec` 可替换为异步读写实现（epoll/io_uring）
- 客户端已具备轻量 reactor 骨架，可演进为 coroutine 驱动 I/O
- `RpcServer` 的连接处理可演进为协程调度
- `ServiceRegistry` 与 handler 签名可平滑扩展为 `future/awaitable`

coroutine API 的后续演进方向：
- 可将 pending slot 与 coroutine handle 直接绑定，减少 future bridge 成本
- 可进一步消除 bridge 线程等待路径，降低上下文切换开销
- 可继续演进为更完整的 coroutine-driven RPC runtime
