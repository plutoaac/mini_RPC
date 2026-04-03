# RPC 框架架构图集

本文档包含 RPC 框架的完整架构图，使用 Mermaid 格式绘制。

---

## 1. 整体架构分层图

```mermaid
flowchart TB
    subgraph BIZ[业务层 Business Layer]
        direction LR
        CLIENT_BIZ[Client Demo<br/>calc::AddRequest]
        SERVER_BIZ[Server Demo<br/>CalcService.Register]
    end

    subgraph FRAMEWORK[框架层 Framework Layer]
        direction TB
        subgraph CLIENT[客户端 Client]
            RC[RpcClient<br/>Call/CallAsync/CallCo]
            EL[EventLoop<br/>epoll+eventfd]
            PC[PendingCalls<br/>request_id → slot]
        end
        subgraph SERVER[服务端 Server]
            RS[RpcServer<br/>Acceptor]
            WL[WorkerLoop<br/>one-loop-per-thread]
            CONN[Connection<br/>协程驱动]
            REG[ServiceRegistry<br/>方法注册表]
        end
    end

    subgraph PROTO[协议层 Protocol Layer]
        CODEC[Codec<br/>[4-byte len][protobuf]]
        RPC_MSG[RpcRequest / RpcResponse]
        BIZ_MSG[业务消息<br/>AddRequest / AddResponse]
    end

    subgraph COROUTINE[协程层 Coroutine Layer]
        TASK[Task<T>]
        AWAITER[FutureAwaiter<br/>ReadAwaiter/WriteAwaiter]
    end

    subgraph COMMON[公共基础层 Common Layer]
        ERR[rpc_error<br/>std::error_code]
        UFD[UniqueFd<br/>RAII fd]
        LOG[Log<br/>std::source_location]
    end

    CLIENT_BIZ --> RC
    SERVER_BIZ --> REG
    
    RC --> EL
    RC --> PC
    RC --> CODEC
    
    RS --> WL
    WL --> CONN
    CONN --> REG
    CONN --> CODEC
    
    RC --> TASK
    CONN --> AWAITER
    
    CODEC --> RPC_MSG
    CODEC --> BIZ_MSG
    
    RC --> ERR
    RS --> ERR
    RC --> UFD
    RS --> UFD
```

---

## 2. 服务端架构详图

```mermaid
flowchart TB
    subgraph MAIN[Main Thread - Acceptor]
        LS[listen_fd<br/>socket/bind/listen]
        AE[accept epoll<br/>epoll_wait]
        ACCEPT[accept4<br/>获取新连接]
        RR[Round-Robin<br/>选择 Worker]
    end

    subgraph W0[Worker Thread 0]
        direction TB
        WL0[WorkerLoop]
        EP0[epoll_fd<br/>连接事件监听]
        WF0[wake_fd<br/>eventfd 唤醒]
        
        subgraph CONNS0[Connections Map]
            C0A[Connection A<br/>协程]
            C0B[Connection B<br/>协程]
        end
    end

    subgraph W1[Worker Thread 1]
        direction TB
        WL1[WorkerLoop]
        EP1[epoll_fd]
        WF1[wake_fd]
        
        subgraph CONNS1[Connections Map]
            C1A[Connection C<br/>协程]
            C1B[Connection D<br/>协程]
        end
    end

    subgraph WN[Worker Thread N]
        WL2[WorkerLoop]
        EP2[epoll_fd]
        WF2[wake_fd]
        CONNS2[...]
    end

    LS --> AE
    AE --> ACCEPT
    ACCEPT --> RR
    RR -->|投递| WL0
    RR -->|投递| WL1
    RR -->|投递| WL2

    WF0 -->|唤醒| EP0
    WF1 -->|唤醒| EP1
    WF2 -->|唤醒| EP2

    EP0 --> C0A
    EP0 --> C0B
    EP1 --> C1A
    EP1 --> C1B
```

---

## 3. 客户端架构详图

```mermaid
flowchart TB
    subgraph USER[用户线程]
        CALL[Call/CallAsync/CallCo]
        FUT[std::future<br/>等待结果]
        CO[协程 Task<br/>co_await]
    end

    subgraph CLIENT[RpcClient]
        SEND[发送请求<br/>write RpcRequest]
        PC[PendingCalls<br/>请求槽管理]
        DISP[Dispatcher Loop<br/>独立线程]
    end

    subgraph EVENT[EventLoop]
        EP[epoll_fd<br/>监听 socket]
        WF[wake_fd<br/>eventfd]
        TO[Timeout Tick<br/>超时检查]
    end

    subgraph NET[网络层]
        SOCK[socket fd<br/>TCP 连接]
    end

    CALL --> SEND
    SEND --> PC
    SEND --> SOCK
    
    PC -->|BindAsync| FUT
    PC -->|BindCoroutine| CO
    
    SOCK --> EP
    WF --> EP
    EP --> DISP
    DISP -->|read response| PC
    PC -->|Complete| FUT
    PC -->|Resume| CO
    
    TO -->|FailTimedOut| PC
```

---

## 4. 请求响应完整时序图

```mermaid
sequenceDiagram
    participant Biz as 业务代码
    participant RC as RpcClient
    participant PC as PendingCalls
    participant EL as EventLoop/Dispatcher
    participant Net as TCP Socket
    participant Sock as Server Socket
    participant WL as WorkerLoop
    participant Conn as Connection
    participant SR as ServiceRegistry
    participant Handler as 业务 Handler

    Note over Biz,Handler: 客户端请求阶段
    Biz->>RC: CallAsync/CallCo(service, method, payload)
    RC->>RC: 生成 request_id
    RC->>PC: Add(request_id)
    RC->>Net: Codec::WriteMessage(RpcRequest)
    RC->>PC: BindAsync/BindCoroutine(request_id, deadline)
    RC-->>Biz: future<RpcCallResult> / Task

    Note over Biz,Handler: 服务端处理阶段
    Sock->>WL: epoll_wait 返回可读
    WL->>Conn: NotifyReadable()
    Conn->>Conn: co_await ReadRequestCo()
    Conn->>Conn: 解析长度前缀 + protobuf
    Conn->>SR: Find(service, method)
    SR-->>Conn: handler
    Conn->>Handler: handler(request_payload)
    Handler-->>Conn: response_payload
    Conn->>Conn: co_await WriteResponseCo()
    Conn->>Sock: write RpcResponse

    Note over Biz,Handler: 客户端响应阶段
    Net->>EL: epoll_wait 返回可读
    EL->>EL: 读取 + 解析 RpcResponse
    EL->>PC: Complete(request_id, result)
    PC->>PC: 查找对应 slot
    PC->>Biz: promise.set_value() / coroutine.resume()
    Biz->>Biz: future.get() / co_await 返回
```

---

## 5. 服务端连接协程流程图

```mermaid
flowchart TB
    START([连接建立]) --> INIT[初始化 Connection]
    INIT --> CO_START[启动 HandleConnectionCo 协程]
    
    CO_START --> LOOP{ShouldClose?}
    LOOP -->|Yes| CLEANUP[清理资源]
    LOOP -->|No| READ[co_await ReadRequestCo]
    
    READ -->|可读| PARSE[解析请求]
    READ -->|超时| TIMEOUT[EnterError<br/>读超时]
    READ -->|对端关闭| CLOSE1[EnterError<br/>EOF]
    
    PARSE --> LOOKUP[查找 ServiceRegistry]
    LOOKUP -->|找到| EXEC[执行 Handler]
    LOOKUP -->|未找到| NOT_FOUND[返回 METHOD_NOT_FOUND]
    
    EXEC -->|成功| BUILD_RESP[构建 RpcResponse]
    EXEC -->|异常| INTERNAL_ERR[返回 INTERNAL_ERROR]
    
    NOT_FOUND --> WRITE
    INTERNAL_ERR --> WRITE
    BUILD_RESP --> WRITE[co_await WriteResponseCo]
    
    WRITE -->|完成| LOOP
    WRITE -->|超时| TIMEOUT2[EnterError<br/>写超时]
    WRITE -->|背压| BACKPRESS[write_buffer 超限]
    
    TIMEOUT --> CLEANUP
    CLOSE1 --> CLEANUP
    TIMEOUT2 --> CLEANUP
    BACKPRESS --> CLEANUP
    
    CLEANUP --> END([连接关闭])
```

---

## 6. PendingCalls 状态管理图

```mermaid
stateDiagram-v2
    [*] --> Empty: 初始化
    
    Empty --> Pending: Add(request_id)
    
    Pending --> Waiting: BindAsync/BindCoroutine
    
    Waiting --> Completed: Complete(request_id, result)
    Waiting --> Timeout: FailTimedOut(request_id)
    Waiting --> Failed: FailAll()
    
    Completed --> [*]: Pop()
    Timeout --> [*]: Pop()
    Failed --> [*]: Pop()
    
    note right of Pending
        请求已发出
        等待绑定 waiter
    end note
    
    note right of Waiting
        有 waiter 等待
        future promise / coroutine handle
    end note
    
    note right of Completed
        响应已到达
        结果已写入
    end note
```

---

## 7. WorkerLoop 事件循环流程

```mermaid
flowchart TB
    START([WorkerLoop.Run]) --> POLL[epoll_wait timeout]
    
    POLL --> TICK[Tick 检查所有连接超时]
    TICK --> EVENT{有事件?}
    
    EVENT -->|无| POLL
    
    EVENT -->|有| PROCESS[遍历 events]
    
    PROCESS --> CHECK{event.fd == wake_fd?}
    
    CHECK -->|Yes| WAKE[读取 wake_fd<br/>取出 pending connections]
    WAKE --> ACCEPT[epoll_ctl ADD client_fd]
    ACCEPT --> START_CO[启动连接协程]
    START_CO --> POLL
    
    CHECK -->|No| IO{事件类型?}
    
    IO -->|EPOLLIN| READ[NotifyReadable]
    IO -->|EPOLLOUT| WRITE[NotifyWritable]
    IO -->|EPOLLHUP/ERR| ERROR[连接错误处理]
    
    READ --> RESUME[恢复读协程]
    WRITE --> RESUME2[恢复写协程]
    ERROR --> CLOSE2[关闭连接]
    
    RESUME --> UPDATE[更新 epoll 事件]
    RESUME2 --> UPDATE
    CLOSE2 --> CLEAN[清理连接 map]
    
    UPDATE --> CLEAN_DONE{协程结束?}
    CLEAN_DONE -->|Yes| CLEAN
    CLEAN_DONE -->|No| POLL
    
    CLEAN --> POLL
```

---

## 8. 协程 Awaiter 挂起恢复流程

```mermaid
sequenceDiagram
    participant Co as 协程
    participant Awaiter as ReadAwaiter/WriteAwaiter
    participant Conn as Connection
    participant Worker as WorkerLoop
    participant EP as epoll

    Note over Co,EP: 协程挂起流程
    Co->>Awaiter: co_await ReadRequestCo()
    Awaiter->>Awaiter: await_ready()
    
    alt 数据已就绪
        Awaiter-->>Co: 直接返回 true，不挂起
    else 需要等待
        Awaiter->>Awaiter: await_ready() = false
        Co->>Awaiter: await_suspend(handle)
        Awaiter->>Conn: 保存 read_waiter_ = handle
        Awaiter->>Conn: ArmReadDeadline()
        Awaiter->>Awaiter: return true (挂起)
        Note over Co: 协程挂起，控制权返回 WorkerLoop
    end

    Note over Co,EP: 事件到达，恢复流程
    Worker->>EP: epoll_wait 返回 EPOLLIN
    Worker->>Conn: NotifyReadable()
    Conn->>Conn: ClearReadDeadline()
    Conn->>Awaiter: ResumeWaiter(read_waiter_)
    Awaiter->>Co: handle.resume()
    
    Note over Co: 协程恢复执行
    Co->>Awaiter: await_resume()
    Awaiter-->>Co: 返回读取结果
```

---

## 9. 多线程分发与线程安全

```mermaid
flowchart TB
    subgraph MT[Main Thread]
        direction TB
        A1[accept 新连接]
        A2[peer_desc = IP:PORT]
        A3[SelectWorker round-robin]
    end

    subgraph WT0[Worker Thread 0]
        Q0[pending_connections_]
        MU0[mutex]
        WF0[wake_fd]
        WL0[WorkerLoop.Run]
    end

    subgraph WT1[Worker Thread 1]
        Q1[pending_connections_]
        MU1[mutex]
        WF1[wake_fd]
        WL1[WorkerLoop.Run]
    end

    A1 --> A2 --> A3
    A3 -->|fd, peer_desc| LOCK0[lock mutex]
    LOCK0 --> PUSH0[push to queue]
    PUSH0 --> UNLOCK0[unlock]
    UNLOCK0 --> WAKE0[write wake_fd = 1]
    
    WAKE0 -.->|跨线程唤醒| WL0
    WL0 --> READ0[read wake_fd]
    READ0 --> LOCK2[lock mutex]
    LOCK2 --> TAKE0[take connections]
    TAKE0 --> UNLOCK2[unlock]
    UNLOCK0 --> REG0[epoll_ctl ADD]
    REG0 --> CO0[启动协程]

    A3 -->|fd, peer_desc| LOCK1[lock mutex]
    LOCK1 --> PUSH1[push to queue]
    PUSH1 --> UNLOCK1[unlock]
    UNLOCK1 --> WAKE1[write wake_fd = 1]
    
    WAKE1 -.->|跨线程唤醒| WL1
    WL1 --> READ1[read wake_fd]
    READ1 --> LOCK3[lock mutex]
    LOCK3 --> TAKE1[take connections]
    TAKE1 --> UNLOCK3[unlock]
    UNLOCK3 --> REG1[epoll_ctl ADD]
    REG1 --> CO1[启动协程]

    style MT fill:#e1f5fe
    style WT0 fill:#fff3e0
    style WT1 fill:#f3e5f5
```

---

## 10. 客户端调用方式对比

```mermaid
flowchart LR
    subgraph SYNC[同步调用 Call]
        S1[Call] --> S2[CallAsync]
        S2 --> S3[future.get]
        S3 --> S4[返回结果]
    end

    subgraph ASYNC[异步调用 CallAsync]
        A1[CallAsync] --> A2[返回 future]
        A2 --> A3[其他工作]
        A3 --> A4[future.get]
        A4 --> A5[返回结果]
    end

    subgraph CO[协程调用 CallCo]
        C1[CallCo] --> C2[返回 Task]
        C2 --> C3[co_await]
        C3 --> C4[挂起协程]
        C4 --> C5[响应到达]
        C5 --> C6[恢复协程]
        C6 --> C7[返回结果]
    end
```

---

## 11. 协议层帧格式

```mermaid
flowchart TB
    subgraph FRAME[RPC 消息帧格式]
        direction LR
        LEN[4 字节长度<br/>big-endian]
        PAYLOAD[Protobuf 数据<br/>RpcRequest/RpcResponse]
    end

    subgraph REQ[RpcRequest 结构]
        R1[request_id: uint32]
        R2[service_name: string]
        R3[method_name: string]
        R4[payload: bytes<br/>业务请求序列化]
    end

    subgraph RESP[RpcResponse 结构]
        RP1[request_id: uint32]
        RP2[error_code: ErrorCode]
        RP3[error_msg: string]
        RP4[payload: bytes<br/>业务响应序列化]
    end

    FRAME --> REQ
    FRAME --> RESP

    subgraph FLOW[编解码流程]
        direction TB
        SEND[发送] --> ENC[Codec::WriteMessage]
        ENC -->|write 4B len| WRITE[write protobuf]
        
        RECV[接收] --> READ[read 4B len]
        READ --> PARSE[Codec::ReadMessage]
        PARSE --> DECODE[解析 protobuf]
    end
```

---

## 12. 错误处理体系

```mermaid
flowchart TB
    subgraph ERRORS[错误类型]
        OK[OK = 0<br/>成功]
        MNF[METHOD_NOT_FOUND = 1<br/>方法不存在]
        PE[PARSE_ERROR = 2<br/>解析失败]
        IE[INTERNAL_ERROR = 3<br/>内部错误]
    end

    subgraph FLOW[错误处理流程]
        direction TB
        CALL[调用请求] --> LOOKUP{查找方法}
        
        LOOKUP -->|找到| PARSE{解析请求}
        LOOKUP -->|未找到| MNF_RESP[返回 METHOD_NOT_FOUND]
        
        PARSE -->|成功| EXEC{执行 Handler}
        PARSE -->|失败| PE_RESP[返回 PARSE_ERROR]
        
        EXEC -->|成功| BUILD[构建响应]
        EXEC -->|异常| IE_RESP[返回 INTERNAL_ERROR]
        
        BUILD --> OK_RESP[返回 OK + payload]
    end

    subgraph CLIENT[客户端错误处理]
        RESP[收到响应] --> CHECK{error_code?}
        CHECK -->|OK| SUCC[解析 payload]
        CHECK -->|非 0| FAIL[返回 Status(error)]
    end

    style OK fill:#c8e6c9
    style MNF fill:#ffcdd2
    style PE fill:#ffcdd2
    style IE fill:#ffcdd2
```

---

## 13. 连接生命周期状态机

```mermaid
stateDiagram-v2
    [*] --> Open: socket accepted
    
    Open --> Reading: co_await ReadRequestCo
    
    Reading --> Reading: 部分数据，继续读
    Reading --> Processing: 完整请求到达
    Reading --> Error: 读超时/连接断开
    
    Processing --> Writing: 处理完成，准备写
    Processing --> Error: handler 异常
    
    Writing --> Writing: 部分写入，继续写
    Writing --> Open: 响应完成，等待下一请求
    Writing --> Error: 写超时/连接断开
    
    Error --> Closing: should_close = true
    Open --> Closing: 客户端关闭
    
    Closing --> Closed: 清理完成
    Closed --> [*]
    
    note right of Open
        连接就绪
        等待请求
    end note
    
    note right of Reading
        等待 socket 可读
        协程可能挂起
    end note
    
    note right of Writing
        等待 socket 可写
        协程可能挂起
    end note
```

---

## 14. 测试覆盖矩阵

```mermaid
flowchart TB
    subgraph UNIT[单元测试]
        direction LR
        U1[codec_test<br/>协议编解码]
        U2[service_registry_test<br/>服务注册]
        U3[pending_calls_test<br/>请求槽管理]
        U4[event_loop_test<br/>事件循环]
    end

    subgraph INTEGRATION[集成测试]
        direction LR
        I1[server_connection_test<br/>连接处理]
        I2[server_worker_loop_test<br/>Worker 循环]
        I3[server_multi_worker_loop_test<br/>多 Worker]
    end

    subgraph E2E[端到端测试]
        direction LR
        E1[e2e_test<br/>基本调用]
        E2[out_of_order_dispatcher_test<br/>乱序响应]
        E3[rpc_server_lifecycle_test<br/>生命周期]
    end

    subgraph COROUTINE[协程测试]
        direction LR
        C1[call_co_basic_test<br/>基本协程调用]
        C2[call_co_timeout_test<br/>协程超时]
        C3[call_co_close_test<br/>连接关闭]
        C4[call_mixed_waiters_test<br/>混合等待]
    end

    subgraph ASYNC[异步测试]
        direction LR
        A1[call_async_timeout_test<br/>异步超时]
        A2[call_async_close_test<br/>异步关闭]
    end

    UNIT --> INTEGRATION
    INTEGRATION --> E2E
    E2E --> COROUTINE
    E2E --> ASYNC
```

---

## 15. 目录结构图

```mermaid
flowchart TB
    ROOT[rpc_project/] --> CMAKE[CMakeLists.txt]
    ROOT --> README[README.md]
    
    ROOT --> SRC[src/]
    SRC --> CLIENT[client/]
    CLIENT --> C1[rpc_client.*]
    CLIENT --> C2[event_loop.*]
    CLIENT --> C3[pending_calls.*]
    CLIENT --> C4[rpc_types.h]
    
    SRC --> SERVER[server/]
    SERVER --> S1[rpc_server.*]
    SERVER --> S2[worker_loop.*]
    SERVER --> S3[connection.*]
    SERVER --> S4[service_registry.*]
    
    SRC --> PROTOCOL[protocol/]
    PROTOCOL --> P1[codec.*]
    
    SRC --> COMMON[common/]
    COMMON --> CO1[rpc_error.h]
    COMMON --> CO2[unique_fd.h]
    COMMON --> CO3[log.h]
    
    SRC --> COROUTINE[coroutine/]
    COROUTINE --> COR1[task.h]
    COROUTINE --> COR2[future_awaiter.h]
    
    SRC --> DEMO[demo/]
    DEMO --> D1[server_main.cpp]
    DEMO --> D2[client_main.cpp]
    DEMO --> D3[coroutine_client_main.cpp]
    
    ROOT --> PROTO[proto/]
    PROTO --> PR1[rpc.proto]
    PROTO --> PR2[calc.proto]
    PROTO --> PR3[user.proto]
    
    ROOT --> TESTS[tests/]
    TESTS --> T1[e2e_test.cpp]
    TESTS --> T2[codec_test.cpp]
    TESTS --> T3[...]
    
    ROOT --> DOCS[docs/]
    DOCS --> DOC1[architecture_diagrams.md]
    DOCS --> DOC2[multi_worker_architecture.md]
    
    ROOT --> BENCH[benchmarks/]
    BENCH --> B1[rpc_benchmark.cpp]
```

---

## 图例说明

| 图表 | 说明 |
|------|------|
| 1. 整体架构分层图 | 展示框架的分层结构和模块依赖关系 |
| 2. 服务端架构详图 | 展示服务端多线程 Worker 架构 |
| 3. 客户端架构详图 | 展示客户端 EventLoop 和请求管理 |
| 4. 请求响应完整时序图 | 展示一次完整 RPC 调用的时序 |
| 5. 服务端连接协程流程图 | 展示连接级协程的处理流程 |
| 6. PendingCalls 状态管理图 | 展示请求槽的状态转换 |
| 7. WorkerLoop 事件循环流程 | 展示 Worker 事件循环细节 |
| 8. 协程 Awaiter 挂起恢复流程 | 展示协程挂起和恢复机制 |
| 9. 多线程分发与线程安全 | 展示跨线程通信机制 |
| 10. 客户端调用方式对比 | 对比三种调用方式 |
| 11. 协议层帧格式 | 展示协议编码格式 |
| 12. 错误处理体系 | 展示错误码和处理流程 |
| 13. 连接生命周期状态机 | 展示连接状态转换 |
| 14. 测试覆盖矩阵 | 展示测试分类和覆盖 |
| 15. 目录结构图 | 展示项目目录结构 |
