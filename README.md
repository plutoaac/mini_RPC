# 可扩展 C++ RPC 框架（最小可运行版本）

本项目实现了一个基于 **C++20 + Protobuf + Linux TCP Socket** 的最小可运行 RPC 框架。

当前版本目标是“闭环可运行 + 分层清晰 + 便于演进”，后续可以平滑升级到：
- C++20 coroutine 版本
- io_uring 异步 IO 版本

## 1. 项目介绍

框架能力（当前版本）：
- 通用 RPC 请求/响应消息（`RpcRequest` / `RpcResponse`）
- 自定义协议：`[4字节长度][protobuf数据]`
- 阻塞式 TCP 通信
- 服务端方法注册与分发（`service_name + method_name -> handler`）
- 客户端通用调用接口
- 基于 `std::error_code` 的统一错误体系
- 基于 RAII 的 socket fd 生命周期管理（`UniqueFd`）
- 基于 `std::source_location` 的轻量结构化日志

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

- `src/server/rpc_server.*`
  - TCP 监听/接收连接（全链路 `UniqueFd` 管理）
  - 解码请求、查找 handler、执行并返回响应
  - 异常与错误统一映射到 `RpcResponse`

- `src/client/rpc_client.*`
  - 连接服务端
  - 构造并发送 `RpcRequest`
  - 接收并解析 `RpcResponse`
  - 通过 `Status(std::error_code + message)` 返回统一错误

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

## 可扩展性说明

当前代码保持阻塞式最小闭环，但接口设计已为后续扩展预留：
- `Codec` 可替换为异步读写实现（epoll/io_uring）
- `RpcServer` 的连接处理可演进为协程调度
- `ServiceRegistry` 与 handler 签名可平滑扩展为 `future/awaitable`
