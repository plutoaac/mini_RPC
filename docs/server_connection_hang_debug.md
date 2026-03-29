# server_connection_test 卡住问题复盘

## 背景

在排查 `server_connection_test` 时，测试会长时间不退出，看起来像“卡住”。
实际复现结果表明，问题不是单一的服务端逻辑错误，而是测试与连接实现中的两个阻塞点叠加导致的。

## 复现方式

在 `build/` 目录下运行：

```bash
timeout 20s ./server_connection_test
```

早期现象是命令一直不结束，最后被 `timeout` 杀掉。

`server_multi_connection_test` 一开始还会因为工作目录不对而找不到 `./rpc_server_demo`，但在 `build/` 目录下可以正常通过。

## 根因分析

### 1. `Connection::Serve()` 默认处理的是阻塞 fd

`server_connection_test` 直接用 `socketpair()` 构造连接，并把一端交给 `rpc::server::Connection::Serve()`。

但 `Serve()` 的读循环是按照非阻塞 socket 设计的：它会持续调用 `recv()`，直到遇到 `EAGAIN`/`EWOULDBLOCK` 才返回。

如果 fd 还是阻塞模式，`recv()` 在数据读完后不会返回 `EAGAIN`，而是继续阻塞等待更多数据，导致线程卡在 `recv()` 内部，`join()` 永远等不到结束。

### 2. 大包回压子测试会把发送端自己堵住

测试最后的回压场景会发送一个 512 KiB 请求体。

之前这一步是直接在主线程里用 `SendAll()` 一次性把完整请求写入 socket，而服务端此时还没开始消费，导致发送端先被阻塞，测试自身进入死锁。

## 修复内容

### 修复 1：让 `Connection::Serve()` 自行切换为非阻塞模式

在进入连接循环之前，先对 fd 执行 `fcntl(F_GETFL)` 和 `fcntl(F_SETFL, O_NONBLOCK)`。

这样 `Serve()` 的语义和内部读写逻辑保持一致，`socketpair()` 这种测试场景也不会再卡在阻塞 `recv()`。

对应代码位置：`src/server/connection.cpp`

### 修复 2：把大包注入改成异步发送

在 `tests/server_connection_test.cpp` 的回压段中，把 `SendAll()` 放到异步任务里执行，同时主线程调用 `OnReadable()` 消费请求。

这样可以避免发送端在测试里先行阻塞，真正验证的是服务器端的分段写回和回压处理。

对应代码位置：`tests/server_connection_test.cpp`

## 验证结果

已验证以下测试通过：

```bash
cd rpc_project/build
timeout 20s ./server_connection_test
timeout 30s ./server_multi_connection_test
```

结果：

- `server_connection_test passed`
- `server_multi_connection_test passed`

## 额外说明

`server_multi_connection_test` 的 child 进程启动命令写死为 `./rpc_server_demo`，因此它依赖当前工作目录是 `build/`。
如果从项目根目录直接运行，测试会先失败于找不到可执行文件，而不是业务逻辑错误。

## 后续建议

1. 如果希望测试更健壮，可以把 `server_multi_connection_test` 的二进制路径改成基于 `argv[0]` 或 CMake 生成路径的相对定位。
2. 如果后续还要继续抓卡住问题，优先看是否存在“阻塞 fd + 非阻塞事件循环”这种模式不一致。