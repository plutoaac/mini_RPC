# RPC Client 流程图文档

本文档详细描述 RPC 客户端的各组件交互和接口调用链。

---

## 1. 整体架构图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              RpcClient                                      │
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌───────────────────────┐          │
│  │  sock_      │    │ write_mu_   │    │  pending_calls_       │          │
│  │  (TCP连接)  │    │ (写串行化)  │    │  (request_id → Slot) │          │
│  └─────────────┘    └─────────────┘    └───────────────────────┘          │
│         │                 │                         │                     │
│         │ send()          │ lock                    │ Add/Bind/Complete   │
│         ▼                 ▼                         ▼                     │
│  ┌─────────────────────────────────────────────────────────────────────┐  │
│  │                        CallAsync / Call / CallCo                     │
│  │  1. Connect() → 建立 TCP 连接                                       │
│  │  2. pending_calls_->Add(request_id)                                │
│  │  3. Codec::WriteMessage() → 发送请求                               │
│  │  4. 绑定等待者 (promise / coroutine_handle)                        │
│  └─────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ dispatcher_thread_ 运行
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                       DispatcherLoop (独立线程)                             │
│                                                                             │
│  ┌───────────────┐    ┌──────────────────────┐    ┌─────────────────────┐  │
│  │  event_loop_  │    │  read_buffer_        │    │  HandleReadable     │
│  │  (epoll)      │    │  (半包缓冲)          │    │  Frames()           │
│  └───────────────┘    └──────────────────────┘    └─────────────────────┘  │
│         │                       │                          │               │
│         │ WaitOnce()            │ 读取/解析                │ 按 request_id │
│         ▼                       ▼                          ▼               │
│  while (running) {                                                        │
│    epoll_wait → kReadable → recv() → 解析帧 → pending_calls_->Complete() │
│    epoll_wait → kTimeout  → pending_calls_->FailTimedOut()               │
│    epoll_wait → kWakeup   → Close() 唤醒                                 │
│    epoll_wait → kError    → pending_calls_->FailAll()                    │
│  }                                                                        │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 客户端连接流程

### 2.1 RpcClient::Connect() 调用链

```
RpcClient::Connect()
    │
    ├── scoped_lock(connect_mu_)          // 保证连接操作线程安全
    │
    ├── if (sock_) → return true          // 已连接，直接返回
    │
    ├── socket(AF_INET, SOCK_STREAM, 0)   // 创建 TCP socket
    │
    ├── inet_pton(AF_INET, host_, &addr)  // 地址转换
    │
    ├── connect(conn_fd, &addr, sizeof)   // 连接服务器
    │
    ├── setsockopt(SO_SNDTIMEO, send_timeout)  // 发送超时
    │
    ├── setsockopt(SO_RCVTIMEO, recv_timeout)  // 接收超时
    │
    ├── sock_ = std::move(conn_fd)        // RAII 接管 socket
    │
    ├── dispatcher_running_ = true
    │
    └── dispatcher_thread_ = std::thread(&RpcClient::DispatcherLoop, this)
            │
            └── 启动 dispatcher 线程，开始监听响应
```

---

## 3. CallAsync 异步调用流程

### 3.1 完整调用链

```
调用线程: client.CallAsync("Service", "Method", payload)
    │
    ├── Connect()                         // 确保连接已建立
    │       │
    │       └── if (!sock_) → 执行 Connect() 流程
    │
    ├── request_id = NextRequestId()      // 原子递增生成唯一 ID
    │
    ├── pending_calls_->Add(request_id)   // 创建空 Slot
    │
    ├── 构建 RpcRequest
    │       │
    │       ├── request.set_request_id(request_id)
    │       ├── request.set_service_name("Service")
    │       ├── request.set_method_name("Method")
    │       └── request.set_payload(payload)
    │
    ├── { scoped_lock(write_mu_) }        // 串行化写入
    │       │
    │       └── Codec::WriteMessage(sock_, request)
    │               │
    │               ├── 序列化 request → string
    │               ├── 写入 4 字节长度头 (network byte order)
    │               └── 写入 body
    │
    ├── 创建 std::promise<RpcCallResult>
    │
    ├── deadline = now() + recv_timeout
    │
    ├── pending_calls_->BindAsync(request_id, promise, deadline)
    │       │
    │       ├── 查找 slots_[request_id]
    │       │
    │       ├── slot.async_promise = std::move(promise)
    │       ├── slot.async_bound = true
    │       ├── slot.deadline = deadline
    │       │
    │       └── if (slot.done) → 立即 set_value 并移除 (快路径)
    │
    └── return promise.get_future()       // 返回 future 给调用者
            │
            └── 调用者可在任意时刻 future.get() 等待结果
```

### 3.2 响应到达时的唤醒路径

```
Dispatcher 线程: 收到服务器响应
    │
    ├── HandleReadableFrames(read_buffer, pending_calls_)
    │       │
    │       ├── 解析 4 字节长度头
    │       ├── 读取 body → RpcResponse
    │       ├── FromProtoErrorCode(response.error_code()) → error_code
    │       │
    │       └── pending_calls_->Complete(request_id, result)
    │               │
    │               ├── 查找 slots_[request_id]
    │               │
    │               ├── slot.done = true
    │               ├── slot.result = result
    │               │
    │               ├── if (slot.async_bound):
    │               │       slot.async_promise->set_value(result)
    │               │
    │               ├── if (slot.coroutine_bound):
    │               │       slot.coroutine_handle.resume()
    │               │
    │               └── slot.cv.notify_all()              // 唤醒 WaitAndPop
    │
    └── slots_.erase(request_id)         // 清理槽位
```

---

## 4. Call 同步调用流程

### 4.1 调用链（基于 CallAsync 封装）

```
调用线程: client.Call("Service", "Method", payload)
    │
    └── CallAsync(service, method, payload).get()
            │
            ├── CallAsync() → 返回 std::future<RpcCallResult>
            │       │
            │       ├── Connect()
            │       ├── Add(request_id)
            │       ├── WriteMessage()
            │       ├── BindAsync(promise)
            │       └── return future
            │
            └── .get() → 阻塞等待响应
                    │
                    ├── 线程在此挂起
                    │
                    ├── Dispatcher 收到响应 → Complete() → set_value()
                    │
                    └── future.get() 返回 RpcCallResult
```

---

## 5. CallCo 协程调用流程

### 5.1 完整调用链

```
协程: co_await client.CallCo("Service", "Method", payload)
    │
    ├── Connect()                         // 确保连接已建立
    │
    ├── request_id = NextRequestId()
    │
    ├── pending_calls_->Add(request_id)   // 创建空 Slot
    │
    ├── 构建 RpcRequest 并发送
    │       │
    │       └── Codec::WriteMessage(sock_, request)
    │
    ├── deadline = now() + recv_timeout
    │
    └── co_await DirectCallCoAwaiter{pending_calls_, request_id, deadline}
            │
            ├── await_ready() → false    // 始终挂起，让 await_suspend 决定
            │
            ├── await_suspend(handle)
            │       │
            │       └── pending_calls_->BindCoroutine(request_id, handle, deadline)
            │               │
            │               ├── 查找 slots_[request_id]
            │               │
            │               ├── 如果响应已到达:
            │               │       → kAlreadyDone → 返回 false (不挂起)
            │               │
            │               ├── 如果响应未到达:
            │               │       → slot.coroutine_handle = handle
            │               │       → slot.coroutine_bound = true
            │               │       → slot.deadline = deadline
            │               │       → kBound → 返回 true (挂起协程)
            │               │
            │               └── 异常: kNotFound / kAlreadyBound → 返回 false
            │
            ├── [协程挂起，等待 Dispatcher 唤醒]
            │       │
            │       └── Dispatcher: Complete() → handle.resume()
            │
            └── await_resume()
                    │
                    ├── 从 pending_calls_->TryPop(request_id) 获取结果
                    │
                    └── 返回 RpcCallResult 给 co_await 表达式
```

### 5.2 DirectCallCoAwaiter 快路径优化

```
场景: 网络极快，响应在协程挂起前已到达

Timeline:
    调用线程                    Dispatcher 线程
    │                              │
    ├── CallCo() 发送请求 ─────────▶│
    │                              ├── recv() 收到响应
    │                              ├── Complete() → slot.done = true
    │                              │
    ├── 执行到 await_suspend()      │
    │   BindCoroutine()             │
    │   → kAlreadyDone (响应已到)   │
    │   → 返回 false (不挂起)       │
    │                              │
    ├── 直接进入 await_resume()     │
    │   → TryPop() 获取结果         │
    │   → 返回                       │
    │                              │
    无挂起/恢复开销，性能最优
```

---

## 6. DispatcherLoop 响应分发流程

### 6.1 主循环

```
DispatcherLoop() [独立线程]
    │
    ├── 获取 sock_fd
    │
    ├── event_loop_ = make_shared<EventLoop>()
    │       │
    │       ├── epoll_create1()
    │       ├── eventfd() (wake_fd_)
    │       └── SetReadFd(sock_fd) → epoll_ctl(ADD, sock_fd, EPOLLIN)
    │
    └── while (dispatcher_running_)
            │
            └── event_loop_->WaitOnce(50ms)
                    │
                    ├── kTimeout (50ms 无可读数据)
                    │       │
                    │       └── pending_calls_->FailTimedOut(now, timeout_result)
                    │               │
                    │               └── 扫描所有 Slot，将到期的标记失败
                    │
                    ├── kWakeup (Close() 主动唤醒)
                    │       │
                    │       └── 不做处理，循环继续检查 running 标志
                    │
                    ├── kError (epoll 错误)
                    │       │
                    │       ├── pending_calls_->FailAll(error_result)
                    │       └── dispatcher_running_ = false
                    │
                    └── kReadable (socket 可读)
                            │
                            ├── 非阻塞 drain 循环读
                            │       │
                            │       └── while (keep_running)
                            │               │
                            │               ├── recv(fd, MSG_DONTWAIT)
                            │               │       │
                            │               │       ├── rc > 0 → append to read_buffer
                            │               │       ├── rc == 0 → peer_closed = true
                            │               │       └── rc < 0 → 检查 errno
                            │               │               ├── EAGAIN/EWOULDBLOCK → break
                            │               │               └── 其他 → 记录错误
                            │
                            ├── HandleReadableFrames(&read_buffer, pending_calls_)
                            │       │
                            │       └── 循环解析完整帧
                            │               │
                            │               ├── 读取 4 字节长度头
                            │               ├── 如果 body 不完整 → break (等下次)
                            │               ├── 解析 RpcResponse
                            │               ├── 构建 RpcCallResult
                            │               └── pending_calls_->Complete(request_id, result)
                            │
                            └── if (peer_closed)
                                    │
                                    └── pending_calls_->FailAll(closed_result)
```

### 6.2 多帧解析流程

```
网络缓冲区: [length1][body1][length2][body2]... [半包...]
    │
    ├── offset = 0
    │
    └── while (buffer.size() - offset >= 4)
            │
            ├── 读取长度头 → body_length
            │
            ├── 检查 body_length 合法性
            │       │
            │       └── if (0 < body_length <= 4MB) → 合法
            │           else → FailAll("invalid frame length")
            │
            ├── if (buffer.size() - offset < 4 + body_length)
            │       │
            │       └── 半包 → break (等待后续可读事件)
            │
            ├── 解析 RpcResponse
            │       │
            │       └── response.ParseFromArray(buffer + offset + 4, body_length)
            │
            ├── pending_calls_->Complete(request_id, result)
            │
            └── offset += 4 + body_length
                    │
                    └── 继续解析下一帧

buffer.erase(0, offset)  // 清除已处理部分
```

---

## 7. PendingCalls 槽位管理

### 7.1 生命周期

```
          Add()                  BindAsync/BindCoroutine           Complete()
Slot:  [空] ──────────▶ [已创建, done=false] ──────────▶ [绑定等待者] ──────▶ [done=true, result设置]
                                                                              │
                                                                              ▼
                                                                        TryPop/WaitAndPop
                                                                        (获取结果并清理)
```

### 7.2 三种等待者的绑定与唤醒

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          PendingCalls::Slot 结构                            │
│                                                                             │
│  done: bool                        // 是否已完成                             │
│  result: RpcCallResult             // 存储结果                               │
│  cv: condition_variable            // 用于 WaitAndPop 阻塞等待               │
│  async_promise: optional<promise>  // CallAsync 的 promise                  │
│  async_bound: bool                 // 是否已绑定异步等待者                   │
│  coroutine_handle: handle          // CallCo 的协程句柄                     │
│  coroutine_bound: bool             // 是否已绑定协程等待者                   │
│  deadline: time_point              // 超时时间点                             │
└─────────────────────────────────────────────────────────────────────────────┘

CallAsync 路径:
    BindAsync() → slot.async_promise = promise, slot.async_bound = true
    Complete()  → slot.async_promise->set_value(result)
    future.get() → 调用者获取结果

Call 路径 (间接使用):
    BindAsync() → slot.async_promise = promise, slot.async_bound = true
    Complete()  → slot.async_promise->set_value(result)
    future.get() → .get() 阻塞等待，调用者获取结果

CallCo 路径:
    BindCoroutine() → slot.coroutine_handle = handle, slot.coroutine_bound = true
    Complete()      → slot.coroutine_handle.resume()
    await_resume()  → TryPop() 获取结果
```

---

## 8. 客户端关闭流程

### 8.1 RpcClient::Close() 调用链

```
RpcClient::Close()
    │
    ├── scoped_lock(connect_mu_)
    │
    ├── dispatcher_running_ = false
    │
    ├── event_loop_->Wakeup()           // 唤醒 epoll_wait
    │
    ├── shutdown(sock_, SHUT_RDWR)      // 半关闭，唤醒阻塞的 recv
    │
    ├── sock_.Reset()                   // 关闭 socket, fd = -1
    │
    ├── dispatcher_thread_.joinable()?
    │       │
    │       └── 是 → to_join = move(dispatcher_thread_)
    │
    └── [锁外]
            │
            ├── to_join.join()          // 等待 dispatcher 线程退出
            │
            └── pending_calls_->FailAll("connection closed")
                    │
                    └── 所有等待中的请求收到错误结果并被唤醒
```

---

## 9. 线程模型总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              线程分布图                                      │
│                                                                             │
│  ┌─────────────────┐                                                        │
│  │   调用线程 (任意)│  ← 用户调用 Call / CallAsync / CallCo 的线程          │
│  │                 │    可多个线程并发发起调用                               │
│  └─────────────────┘                                                        │
│           │                                                                 │
│           │ Add() / BindAsync() / BindCoroutine() / future.get()           │
│           ▼                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐  │
│  │                      PendingCalls (线程安全)                         │  │
│  │   slots_: { request_id → Slot{done, result, cv, promise, handle} } │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
│           ▲                              │                                │
│           │ Complete() / FailAll()       │ Add() / BindAsync()            │
│           │                              ▼                                │
│  ┌─────────────────┐          ┌─────────────────┐                        │
│  │ Dispatcher 线程  │◄────────│  调用线程        │                        │
│  │                 │          │                 │                        │
│  │ epoll_wait      │          │ write_mu_ 保护  │                        │
│  │ recv() / 解析   │          │ Codec::Write    │                        │
│  │ Complete()      │          │                 │                        │
│  └─────────────────┘          └─────────────────┘                        │
│                                                                             │
│  交互关系:                                                                  │
│  - 调用线程: 写入请求 → pending_calls_ 注册等待 → 等待响应                  │
│  - Dispatcher: 读取响应 → 解析帧 → pending_calls_ 完成并唤醒               │
│  - write_mu_: 保证多调用线程并发 Call 时写入串行化                          │
│  - PendingCalls: 唯一桥梁，实现请求-响应的 request_id 路由                  │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 10. 接口调用速查表

| 场景 | 调用链 |
|------|--------|
| 首次调用 | `Call/CallAsync/CallCo` → `Connect()` → socket + epoll + dispatcher thread |
| 发起请求 | `Add()` → `WriteMessage()` → `BindAsync/BindCoroutine()` → 返回 future/Task |
| 响应接收 | `epoll_wait` → `recv()` → `HandleReadableFrames()` → `Complete()` → 唤醒等待者 |
| 超时清理 | `WaitOnce` 返回 kTimeout → `FailTimedOut()` → 扫描到期 Slot |
| 连接关闭 | `Close()` → `shutdown()` → `Wakeup()` → `join()` → `FailAll()` |
| 客户端析构 | `~RpcClient()` → 自动调用 `Close()` |
| 多帧解析 | `recv()` → `read_buffer` → 循环解析 → 半包缓存 → `Complete()` × N |

---

## 11. 关键数据结构关系

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                RpcClient                                    │
│                                                                             │
│  sock_: UniqueFd                    // 唯一 TCP 连接                        │
│  pending_calls_: shared_ptr         // 请求管理器                           │
│  event_loop_: shared_ptr            // dispatcher 的事件循环                │
│  dispatcher_thread_: std::thread    // 响应分发线程                         │
│  connect_mu_: mutex                 // 保护连接/关闭操作                    │
│  write_mu_: mutex                   // 串行化写入                          │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ 持有
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              PendingCalls                                   │
│                                                                             │
│  slots_: unordered_map<string, Slot>                                       │
│                                                                             │
│  Slot {                                                                    │
│    done: bool                                                              │
│    result: RpcCallResult                                                   │
│    cv: condition_variable                                                  │
│    async_promise: optional<promise<RpcCallResult>>                        │
│    coroutine_handle: coroutine_handle<>                                    │
│    deadline: time_point                                                    │
│  }                                                                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ 持有
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                               EventLoop                                     │
│                                                                             │
│  epoll_fd_: UniqueFd                // epoll 实例                           │
│  wake_fd_: UniqueFd                 // eventfd 用于唤醒                     │
│  read_fd_: int                      // 被监听的业务 socket fd              │
└─────────────────────────────────────────────────────────────────────────────┘
```
