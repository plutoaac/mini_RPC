# RPC Server 流程图文档

本文档详细描述 RPC 服务端的各组件交互和接口调用链。

---

## 1. 整体架构图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              RpcServer (Acceptor)                           │
│                                                                             │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐                     │
│  │ listen_fd_  │    │ epoll_fd_   │    │ workers_[]  │                     │
│  │ (监听端口)  │    │ (acceptor)  │    │ (WorkerLoop)│                     │
│  └─────────────┘    └─────────────┘    └─────────────┘                     │
│         │                 │                   │                            │
│         │ accept()        │ epoll_wait        │ round-robin               │
│         ▼                 ▼                   ▼                            │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                        主线程事件循环                                │   │
│  │  while (!stop_requested_) { epoll_wait(); accept(); dispatch(); }   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
              ┌───────────────────────┼───────────────────────┐
              ▼                       ▼                       ▼
┌─────────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐
│    WorkerLoop 0     │   │    WorkerLoop 1     │   │    WorkerLoop N     │
│                     │   │                     │   │                     │
│  ┌───────────────┐  │   │  ┌───────────────┐  │   │  ┌───────────────┐  │
│  │  epoll_fd_    │  │   │  │  epoll_fd_    │  │   │  │  epoll_fd_    │  │
│  └───────────────┘  │   │  └───────────────┘  │   │  └───────────────┘  │
│  ┌───────────────┐  │   │  ┌───────────────┐  │   │  ┌───────────────┐  │
│  │  wake_fd_     │  │   │  │  wake_fd_     │  │   │  │  wake_fd_     │  │
│  └───────────────┘  │   │  └───────────────┘  │   │  └───────────────┘  │
│  ┌───────────────┐  │   │  ┌───────────────┐  │   │  ┌───────────────┐  │
│  │ connections_  │  │   │  │ connections_  │  │   │  │ connections_  │  │
│  │  fd → state  │  │   │  │  fd → state  │  │   │  │  fd → state  │  │
│  └───────────────┘  │   │  └───────────────┘  │   │  └───────────────┘  │
│  ┌───────────────┐  │   │  ┌───────────────┐  │   │  ┌───────────────┐  │
│  │ completed_    │  │   │  │ completed_    │  │   │  │ completed_    │  │
│  │   queue_      │  │   │  │   queue_      │  │   │  │   queue_      │  │
│  └───────────────┘  │   │  └───────────────┘  │   │  └───────────────┘  │
└─────────────────────┘   └─────────────────────┘   └─────────────────────┘
          │                         │                         │
          ▼                         ▼                         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         ThreadPool (业务线程池)                             │
│                                                                             │
│   Thread 0        Thread 1        Thread 2        Thread 3                 │
│   [handler]       [handler]       [handler]       [handler]                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 服务启动流程

### 2.1 RpcServer::Start() 调用链

```
RpcServer::Start()
    │
    ├── InitAcceptor()
    │       │
    │       ├── socket(AF_INET, SOCK_STREAM, 0)        // 创建 TCP socket
    │       │
    │       ├── setsockopt(SO_REUSEADDR)               // 设置地址复用
    │       │
    │       ├── fcntl(O_NONBLOCK)                      // 设置非阻塞
    │       │
    │       ├── bind(port)                             // 绑定端口
    │       │
    │       ├── listen(backlog=128)                    // 开始监听
    │       │
    │       ├── epoll_create1(EPOLL_CLOEXEC)           // 创建 acceptor epoll
    │       │
    │       └── epoll_ctl(EPOLL_CTL_ADD, listen_fd)    // 注册监听 fd
    │
    ├── StartBusinessThreadPool()
    │       │
    │       └── ThreadPool::Start(thread_count)        // 启动业务线程池
    │
    ├── StartWorkers()
    │       │
    │       ├── for i in 0..worker_count:
    │       │       │
    │       │       ├── new WorkerLoop(i, registry, thread_pool)
    │       │       │
    │       │       ├── WorkerLoop::Init()
    │       │       │       │
    │       │       │       ├── epoll_create1()        // worker 的 epoll
    │       │       │       │
    │       │       │       ├── eventfd()              // wake fd
    │       │       │       │
    │       │       │       ├── epoll_ctl(ADD, wake_fd)
    │       │       │       │
    │       │       │       └── completed_queue_ = make_shared<CompletedQueue>()
    │       │       │
    │       │       └── WorkerLoop::Start()
    │       │               │
    │       │               └── thread_ = std::thread(Run)  // 启动工作线程
    │       │
    │       └── accepting_new_connections_ = true
    │
    └── 主事件循环
            │
            └── while (!stop_requested_)
                    │
                    ├── epoll_wait(accept_epoll_fd_)
                    │
                    ├── accept4(listen_fd, SOCK_NONBLOCK)
                    │
                    ├── SelectWorker()  // round-robin 选择 worker
                    │
                    └── WorkerLoop::EnqueueConnection(fd, peer_desc)
```

---

## 3. 连接接入流程

### 3.1 新连接分发

```
客户端连接请求
        │
        ▼
┌───────────────────────────────────────────────────────────────────┐
│                      RpcServer 主线程                             │
│                                                                   │
│  epoll_wait() 返回 EPOLLIN on listen_fd_                         │
│       │                                                           │
│       ▼                                                           │
│  accept4(listen_fd, SOCK_NONBLOCK | SOCK_CLOEXEC)                │
│       │                                                           │
│       ├── 获取 client_fd 和 peer_addr                             │
│       │                                                           │
│       ▼                                                           │
│  SelectWorker()  // round-robin                                   │
│       │                                                           │
│       │   next_worker_ = (next_worker_ + 1) % worker_count_      │
│       │                                                           │
│       ▼                                                           │
│  workers_[next_worker_]->EnqueueConnection(fd, peer_desc)        │
│       │                                                           │
│       │   ┌─────────────────────────────────────────────────┐     │
│       │   │ WorkerLoop::EnqueueConnection()                  │     │
│       │   │                                                 │     │
│       │   │   {                                             │     │
│       │   │     lock_guard(pending_mutex_);                 │     │
│       │   │     pending_connections_.push_back({fd, desc}); │     │
│       │   │   }                                             │     │
│       │   │                                                 │     │
│       │   │   Wakeup()  // 写 eventfd 唤醒 worker           │     │
│       │   └─────────────────────────────────────────────────┘     │
│       │                                                           │
│       └── 返回，继续 epoll_wait                                   │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## 4. WorkerLoop 事件循环

### 4.1 Run() 主循环

```
WorkerLoop::Run()  [工作线程]
    │
    └── while (!stop_requested_)
            │
            └── PollOnce(timeout_ms)
                    │
                    ├── epoll_wait(epoll_fd_, events, timeout)
                    │
                    ├── 检查 wake_fd 事件
                    │       │
                    │       ├── DrainWakeFd()
                    │       │
                    │       ├── DrainPendingConnections()
                    │       │       │
                    │       │       └── for each pending:
                    │       │               AddConnectionOnOwnerThread()
                    │       │
                    │       └── DrainCompletedResponses()
                    │               │
                    │               └── for each completed:
                    │                       发送响应给客户端
                    │
                    ├── 检查所有连接超时
                    │       │
                    │       └── for each connection:
                    │               Connection::Tick(now)
                    │
                    └── 处理就绪事件
                            │
                            ├── EPOLLIN  → Connection::NotifyReadable()
                            ├── EPOLLOUT → Connection::NotifyWritable()
                            ├── EPOLLHUP/EPOLLERR → Connection::MarkClosing()
                            │
                            └── UpdateEpollInterest()
```

### 4.2 连接接管流程

```
WorkerLoop::AddConnectionOnOwnerThread(fd, peer_desc)
    │
    ├── epoll_ctl(EPOLL_CTL_ADD, fd, EPOLLIN|EPOLLRDHUP)
    │
    ├── 创建 ConnectionState
    │       │
    │       ├── connection_token = next_connection_token_.fetch_add(1)
    │       │
    │       ├── Connection::BindToWorkerLoop(worker_id)
    │       │
    │       └── if (thread_pool_):
    │               Connection::SetRequestDispatcher(dispatcher)
    │
    ├── connections_.emplace(fd, state)
    │
    └── StartConnectionCoroutine(state)
            │
            └── state->task = HandleConnectionCo(&connection, &ok, &error)
```

---

## 5. 协程处理流程

### 5.1 HandleConnectionCo 协程

```
HandleConnectionCo(connection, coroutine_ok, coroutine_error)
    │
    └── while (true)
            │
            ├── 检查 ShouldClose() → co_return
            │
            ├── co_await ReadRequestCo()
            │       │
            │       ├── co_await WaitReadableCo()  // 挂起点
            │       │       │
            │       │       └── 等待 socket 可读
            │       │
            │       └── OnReadable()
            │               │
            │               ├── ReadFromSocket()
            │               │
            │               └── TryParseRequests()
            │
            ├── 检查 ShouldClose() → co_return
            │
            └── while (HasPendingWrite())
                    │
                    ├── co_await WriteResponseCo()
                    │       │
                    │       ├── co_await WaitWritableCo()  // 挂起点
                    │       │       │
                    │       │       └── 等待 socket 可写
                    │       │
                    │       └── OnWritable()
                    │               │
                    │               └── FlushWrites()
                    │
                    └── 检查 ShouldClose() → co_return
```

---

## 6. 线程池模式请求处理

### 6.1 请求分发到线程池

```
Connection::TryParseRequests()
    │
    └── 解析成功后
            │
            ├── if (request_dispatcher_)
            │       │
            │       └── request_dispatcher_(dispatch, error_msg)
            │               │
            │               └── WorkerLoop::DispatchRequestToThreadPool()
            │                       │
            │                       ├── thread_pool_->Submit([lambda])
            │                       │       │
            │                       │       └── [业务线程执行]
            │                       │               │
            │                       │               ├── 查找 handler
            │                       │               │
            │                       │               ├── 执行 handler(payload)
            │                       │               │
            │                       │               ├── RecordMethodCall()
            │                       │               │
            │                       │               ├── completed_queue_->responses.push_back()
            │                       │               │
            │                       │               └── write(wake_fd)  // 唤醒 worker
            │                       │
            │                       ├── submitted_request_count_++
            │                       │
            │                       └── in_flight_request_count_++
            │
            └── else (无线程池)
                    │
                    └── HandleOneRequest()  // 同步处理
```

### 6.2 响应回投流程

```
线程池完成处理
    │
    ├── 构建 CompletedResponse { fd, token, request_id, payload, ... }
    │
    ├── {
    │     lock_guard(completed_queue_->mutex);
    │     completed_queue_->responses.push_back(completed);
    │   }
    │
    └── write(wake_fd, 1)  // 唤醒 worker 线程
            │
            ▼
┌───────────────────────────────────────────────────────────────────┐
│                     WorkerLoop 工作线程                           │
│                                                                   │
│  epoll_wait() 返回 wake_fd 可读                                  │
│       │                                                           │
       ▼                                                           │
│  DrainCompletedResponses()                                       │
│       │                                                           │
       ├── local.swap(completed_queue_->responses)  // 批量取出    │
│       │                                                           │
       ├── for each completed:                                     │
│       │       │                                                   │
│       │       ├── in_flight_request_count_--                     │
│       │       │                                                   │
│       │       ├── 查找 connections_[fd]                          │
│       │       │                                                   │
│       │       ├── 验证 connection_token                          │
│       │       │                                                   │
│       │       ├── connection.EnqueueResponse(response)           │
│       │       │                                                   │
│       │       ├── connection.NotifyReadable()  // 唤醒协程       │
│       │       │                                                   │
│       │       └── UpdateEpollInterest()  // 可能需要 EPOLLOUT   │
│       │                                                           │
│       └── return true                                            │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

---

## 7. 连接关闭流程

### 7.1 正常关闭

```
WorkerLoop::CloseConnection(fd, reason)
    │
    ├── 查找 connections_[fd]
    │
    ├── Connection::MarkClosing()
    │       │
    │       ├── state_ = State::kClosing
    │       ├── should_close_ = true
    │       ├── ClearReadDeadline()
    │       ├── ClearWriteDeadline()
    │       └── ResumeWaiter()  // 唤醒挂起的协程
    │
    ├── Connection::NotifyReadable()   // 唤醒读等待
    ├── Connection::NotifyWritable()   // 唤醒写等待
    │
    ├── task->Get()  // 等待协程结束
    │
    ├── epoll_ctl(EPOLL_CTL_DEL, fd)   // 从 epoll 移除
    │
    └── connections_.erase(it)  // 移除连接，触发析构
            │
            └── ConnectionState 析构
                    │
                    └── Connection 析构 → UniqueFd 析构 → close(fd)
```

### 7.2 服务停止流程

```
RpcServer::Stop()
    │
    ├── stop_requested_ = true
    ├── accepting_new_connections_ = false
    │
    ├── StopWorkers()
    │       │
    │       └── for each worker:
    │               │
    │               ├── WorkerLoop::RequestStop()
    │               │       │
    │               │       ├── accepting_new_connections_ = false
    │               │       ├── stop_requested_ = true
    │               │       └── Wakeup()
    │               │
    │               └── WorkerLoop::Join()
    │                       │
    │                       └── thread_.join()
    │
    ├── StopBusinessThreadPool()
    │
    └── CloseAcceptor()
            │
            ├── epoll_ctl(DEL, listen_fd)
            └── close(listen_fd)
```

---

## 8. 关键数据结构关系

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                RpcServer                                    │
│                                                                             │
│  workers_: vector<unique_ptr<WorkerLoop>>                                  │
│  business_thread_pool_: unique_ptr<ThreadPool>                             │
│  registry_: const ServiceRegistry&                                         │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ 持有
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                                WorkerLoop                                   │
│                                                                             │
│  connections_: unordered_map<int, unique_ptr<ConnectionState>>             │
│  completed_queue_: shared_ptr<CompletedQueue>                              │
│  thread_pool_: ThreadPool*  (借用，不持有)                                  │
│  registry_: const ServiceRegistry& (借用)                                  │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ 持有
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                              ConnectionState                                │
│                                                                             │
│  connection: Connection                                                    │
│  task: optional<Task<void>>                                                │
│  connection_token: uint64_t                                                │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      │ 持有
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                                 Connection                                  │
│                                                                             │
│  fd_: UniqueFd                                                             │
│  read_buffer_: string                                                      │
│  write_buffer_: string                                                     │
│  request_dispatcher_: function<...>  (可选，指向 WorkerLoop 方法)          │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 9. 线程模型总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              线程分布图                                      │
│                                                                             │
│  ┌─────────────────┐                                                        │
│  │   Main Thread   │  ← RpcServer::Start() 运行在此                        │
│  │   (Acceptor)    │    负责 accept + 分发                                  │
│  └─────────────────┘                                                        │
│           │                                                                 │
│           │ EnqueueConnection()                                             │
│           ▼                                                                 │
│  ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐          │
│  │  Worker Thread 0│   │  Worker Thread 1│   │  Worker Thread N│          │
│  │                 │   │                 │   │                 │          │
│  │ WorkerLoop::Run │   │ WorkerLoop::Run │   │ WorkerLoop::Run │          │
│  │   epoll_wait    │   │   epoll_wait    │   │   epoll_wait    │          │
│  │   协程调度      │   │   协程调度      │   │   协程调度      │          │
│  └─────────────────┘   └─────────────────┘   └─────────────────┘          │
│           │                   │                     │                      │
│           │ Submit()          │                     │                      │
│           ▼                   ▼                     ▼                      │
│  ┌─────────────────────────────────────────────────────────────────────┐  │
│  │                      Business Thread Pool                            │  │
│  │                                                                      │  │
│  │   Thread 0      Thread 1      Thread 2      Thread 3               │  │
│  │   [handler]     [handler]     [handler]     [handler]              │  │
│  │                                                                      │  │
│  │   执行用户注册的业务方法                                            │  │
│  │   完成后将结果写入 completed_queue_                                 │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 10. 接口调用速查表

| 场景 | 调用链 |
|------|--------|
| 服务启动 | `RpcServer::Start()` → `InitAcceptor()` → `StartWorkers()` → 主循环 |
| 新连接接入 | `epoll_wait` → `accept4()` → `SelectWorker()` → `EnqueueConnection()` |
| 连接接管 | `DrainPendingConnections()` → `AddConnectionOnOwnerThread()` → `StartConnectionCoroutine()` |
| 请求读取 | `HandleConnectionCo` → `ReadRequestCo()` → `WaitReadableCo()` → `OnReadable()` |
| 同步处理 | `TryParseRequests()` → `HandleOneRequest()` → `EnqueueResponse()` |
| 异步处理 | `TryParseRequests()` → `DispatchRequestToThreadPool()` → 线程池执行 |
| 响应发送 | `DrainCompletedResponses()` → `EnqueueResponse()` → `NotifyReadable()` |
| 连接关闭 | `CloseConnection()` → `MarkClosing()` → 协程退出 → `connections_.erase()` |
| 服务停止 | `Stop()` → `StopWorkers()` → `Join()` → `CloseAcceptor()` |
