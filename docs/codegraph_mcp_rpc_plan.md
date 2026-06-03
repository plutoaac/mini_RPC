# CodeGraph MCP over mini_rpc Plan

## 目标

把 CodeGraph 的 MCP 能力拆成两层：

- 本地 MCP adapter：继续对 AI Agent 暴露标准 stdio JSON-RPC MCP 协议。
- 远端 CodeGraph RPC service：通过 mini_rpc 提供查询、上下文、调用关系、状态等能力。

这样 Claude、Codex 等 Agent 仍按 MCP 方式接入，但多个 Agent 可以共享一个远端索引服务。

## 目标架构

```text
AI Agent
    |
    | stdio JSON-RPC (MCP)
    v
MCP adapter
    |
    | mini_rpc TCP + RpcRequest/RpcResponse
    v
CodeGraph RPC Server
    |
    v
CodeGraph backend / index database
```

## 边界划分

### MCP adapter

职责：

- 读取 stdin 上的 MCP JSON-RPC 请求。
- 处理 `initialize`、`tools/list`、`tools/call`。
- 把 `tools/call` 转成 mini_rpc 调用。
- 把 mini_rpc 返回值包装回 MCP `content: [{type: "text", text: ...}]`。

不负责：

- 不直接读写索引数据库。
- 不实现 CodeGraph 查询逻辑。
- 不做重型解析、遍历、索引更新。

### CodeGraph RPC Server

职责：

- 启动 `rpc::server::RpcServer`。
- 注册 `CodeGraph` 服务下的方法。
- 调用 CodeGraph backend 完成实际查询。
- 返回 JSON 字符串或未来的 protobuf response。

建议首版注册方法：

- `CodeGraph.Search`
- `CodeGraph.Context`
- `CodeGraph.Callers`
- `CodeGraph.Callees`
- `CodeGraph.Impact`
- `CodeGraph.Node`
- `CodeGraph.Status`
- `CodeGraph.Files`

### mini_rpc

职责：

- 提供 TCP 传输、请求/响应关联、并发处理、连接池和负载均衡。
- 复用现有 `RpcRequest { service_name, method_name, payload }`。
- 复用现有 `ServiceRegistry::Register` handler 模型。

## 首版协议

首版不要新增业务 protobuf，直接使用 JSON payload，降低集成成本。

请求：

```text
service_name = "CodeGraph"
method_name  = "Search" | "Context" | "Callers" | ...
payload      = JSON string bytes
```

响应：

```text
payload = JSON string bytes
```

示例：

```json
{
  "query": "RpcClient",
  "limit": 20
}
```

后续如果性能或 schema 稳定性成为问题，再新增 `proto/codegraph.proto`。

## MCP Tool 映射

| MCP tool | mini_rpc service | mini_rpc method | payload |
|---|---|---|---|
| `codegraph_search` | `CodeGraph` | `Search` | `{"query": "...", "limit": 20}` |
| `codegraph_context` | `CodeGraph` | `Context` | `{"symbol": "...", "limit": 10}` |
| `codegraph_callers` | `CodeGraph` | `Callers` | `{"symbol": "...", "max_depth": 3}` |
| `codegraph_callees` | `CodeGraph` | `Callees` | `{"symbol": "...", "max_depth": 3}` |
| `codegraph_impact` | `CodeGraph` | `Impact` | `{"symbol": "...", "max_depth": 5}` |
| `codegraph_node` | `CodeGraph` | `Node` | `{"symbol": "..."}` |
| `codegraph_status` | `CodeGraph` | `Status` | `{}` |
| `codegraph_files` | `CodeGraph` | `Files` | `{}` |

## 实施阶段

### Phase 1: RPC service skeleton（已完成）

新增一个 CodeGraph RPC server 可执行文件。

建议文件：

- `mini_rpc/src/demo/codegraph_rpc_server_main.cpp` 或后续独立到 `codegraph-cpp/src/rpc/`

内容：

- 创建 `ServiceRegistry`。
- 注册 `CodeGraph.Search` 等方法。
- 早期 handler 用 mock JSON 验证 mini_rpc 链路；当前已接入 `SQLiteCodeGraphBackend` 和 `CodeGraphNativeBackend`。
- 启动 `RpcServer(port, registry, worker_count, business_thread_count)`。

验收：

- 用 `RpcClient` 调 `CodeGraph.Status` 能拿到 JSON 响应。
- server stats 能看到方法调用计数。

### Phase 2: MCP adapter skeleton（已完成）

新增本地 MCP adapter 可执行文件。

建议职责：

- 从 stdin 按行读取 JSON-RPC。
- 支持 `initialize`。
- 支持 `tools/list`。
- 支持 `tools/call`，并转发到 mini_rpc。

首版只需要同步调用：

```cpp
client.Call("CodeGraph", mapped_method, args.dump());
```

验收：

- 手写一条 MCP `initialize` 请求，返回合法 MCP response。
- 手写一条 `tools/list` 请求，返回工具列表。
- 手写一条 `tools/call codegraph_status`，adapter 通过 mini_rpc 返回远端结果。

### Phase 3: 接入 CodeGraph backend（已完成）

当前支持两种 backend：

- `--backend native`：直接调用 `codegraph-cpp::Database / ContextBuilder / GraphTraverser`，复用 codegraph-cpp 的搜索、上下文和图遍历逻辑。
- `--backend sqlite`：schema adapter，兼容 official CodeGraph schema 和 `codegraph-cpp` 的 `.codegraph/index` schema。

当前推荐 native backend；sqlite backend 保留为兼容/对照路径。

### Phase 4: 并发与连接池

MCP adapter 可以从单 `RpcClient` 升级成 `RpcClientPool`。

配置：

- 本地开发：一个 endpoint。
- 多实例部署：多个 endpoint，使用 `kLeastInflight`。

验收：

- 一个 adapter 可连接多个 CodeGraph RPC server。
- 某个 endpoint 失败时，同步调用能重试到其它健康节点。

### Phase 5: schema 固化

如果 JSON payload 已稳定，再考虑新增：

- `proto/codegraph.proto`
- `SearchRequest/SearchResponse`
- `ContextRequest/ContextResponse`
- `Node/Edge/File` message

收益：

- 更强类型。
- 更低解析成本。
- 更适合跨语言 client。

代价：

- 每次接口变化都要更新 proto 和生成代码。
- MCP adapter 仍需要 JSON <-> protobuf 转换。

## 当前落地状态

### 已完成

- `CodeGraph` RPC service 已注册 `Search / Context / Callers / Callees / Impact / Node / Status / Files`。
- MCP adapter 已支持 `initialize`、`tools/list`、`tools/call`，并通过 mini_rpc 转发。
- `SQLiteCodeGraphBackend` 支持 official CodeGraph schema 和 `codegraph-cpp` schema。
- `CodeGraphNativeBackend` 已接入 `codegraph-cpp::Database / GraphTraverser / ContextBuilder`，可通过 `--backend native` 启用。
- `codegraph-cpp` 建边已修复：临时 node id 会映射到真实 SQLite id，批量插入调用边并保留 unresolved refs。
- 已有 `codegraph_mcp_rpc_test` 和 `codegraph_native_backend_test` 覆盖 MCP -> mini_rpc -> backend 的主链路。

### 剩余风险

#### SQLite 并发

`mini_rpc` 的业务 handler 可能在线程池中并发执行。不要让多个线程共享一个无保护的 `sqlite3*`。

当前策略：

- native backend 每次 `Invoke()` 打开独立 `Database` connection。
- sqlite backend 每次查询打开只读 connection，并设置 `busy_timeout`。
- 索引写入仍由 `codegraph-cpp` CLI/watch 负责，RPC 服务当前定位为查询服务。

#### MCP adapter 不要变厚

adapter 只做协议转换。所有业务逻辑都放远端 RPC service，否则后续多 Agent 共享索引的收益会被削弱。

## 验证方式

```bash
cd /root/RPC_pro/mini_rpc/build
ctest --output-on-failure -R 'codegraph_mcp_rpc_test|codegraph_native_backend_test'
```

也可以手动启动：

```bash
./codegraph_rpc_server_demo --backend native --db /path/to/.codegraph/index --port 50061
./codegraph_mcp_adapter_demo --host 127.0.0.1 --port 50061
```

## 推荐目录演进

短期放在 `mini_rpc` 里验证传输层：

```text
mini_rpc/
  src/demo/codegraph_rpc_server_main.cpp
  src/demo/codegraph_mcp_adapter_main.cpp
```

中期拆回 CodeGraph 项目边界：

```text
codegraph-cpp/
  src/rpc/codegraph_rpc_service.cpp
  src/rpc/codegraph_rpc_server_main.cpp
  src/mcp/mcp_rpc_adapter.cpp
```

`mini_rpc` 保持为纯 RPC 框架，不长期承载 CodeGraph 业务代码。
