# CodeGraph + Mini-RPC Integration Architecture

## System Architecture

```
+------------------------------------------------------------------+
|                        Claude Code / Cursor / 任意 MCP Client     |
|                                                                  |
|   "帮我看看 CppExtractor 的调用者是谁"                             |
|   "如果改了 Database::get_node 影响范围多大"                       |
+------------------------------------------------------------------+
          |  stdin/stdout JSON-RPC (MCP Protocol)
          v
+------------------------------------------------------------------+
|                     MCP Adapter Layer                             |
|                                                                  |
|   方式 A: codegraph serve --mcp  (直连, 无网络)                    |
|   方式 B: codegraph_mcp_adapter  (经 mini-rpc TCP 转发)           |
+------------------------------------------------------------------+
          |                                    |
          | (方式A: 直接调用)                    | (方式B: TCP RPC)
          v                                    v
+-----------------------+     +------------------------------------+
|   codegraph-cpp CLI   |     |        mini-rpc RPC Server         |
|                       |     |                                    |
|  Database             |     |  RpcServer (epoll + worker threads)|
|  GraphTraverser       |     |    |                               |
|  ContextBuilder       |     |    +-> ServiceRegistry             |
|  FtsSearch            |     |         |                          |
|                       |     |         +-> CodeGraph.<method>     |
|  SQLite (FTS5)        |     |              |                     |
+-----------------------+     |              v                     |
          |                   |     CodeGraphBackend               |
          |                   |     - native: codegraph-cpp core   |
          |                   |     - sqlite: schema adapter       |
          |                   |              |                     |
          +-------------------+--------------+                     |
                             |                                     |
                             v                                     |
                    +------------------+                           |
                    |  SQLite Database |                           |
                    |  (.codegraph/    |                           |
                    |   index)         |                           |
                    +------------------+                           |
                                                                   |
+------------------------------------------------------------------+
|                     mini-rpc Framework Core                       |
|                                                                  |
|  Protocol: 4-byte length-prefix + Protobuf                       |
|  Transport: epoll non-blocking I/O                               |
|  Threading: one-loop-per-thread (Acceptor -> WorkerLoop)         |
|  Coroutine: C++20 Task<T> with symmetric transfer                |
|  Logging: Async MPSC ring buffer                                 |
+------------------------------------------------------------------+
```

## Data Flow: MCP Tool Call

```
User: "search for CppExtractor"
  |
  v
Claude Code sends JSON-RPC:
  {"method":"tools/call","params":{"name":"codegraph_search","arguments":{"query":"CppExtractor"}}}
  |
  v
MCP Adapter parses -> maps tool name to RPC method "CodeGraph.Search"
  |
  v
mini-rpc serializes as Protobuf RpcRequest {service:"CodeGraph", method:"Search", ...}
  |
  v
TcpClient sends 4-byte length-prefix frame
  |
  v
RpcServer WorkerLoop epoll_wait -> read frame -> deserialize
  |
  v
ServiceRegistry lookup "CodeGraph.Search" -> handler lambda
  |
  v
CodeGraphBackend::Invoke("Search", {query:"CppExtractor"})
  |
  v
native backend: ContextBuilder/Search over codegraph-cpp Database
or sqlite backend: schema-aware SQL query
  |
  v
Results -> JSON -> Protobuf RpcResponse -> TCP -> MCP Adapter -> JSON-RPC -> Claude Code
```

## Database Schema (codegraph-cpp)

```sql
-- 16 columns, INTEGER kind, FTS5 full-text search
nodes(id, kind, name, qualified_name, file_path, language,
      line, col, end_line, end_col,
      signature, docstring, visibility,
      is_static, is_const, is_exported)

-- INTEGER kind, foreign keys to nodes
edges(id, source_id, target_id, kind, line, col, metadata)

-- file tracking for incremental indexing
files(id, path, language, mtime, size)

-- FTS5 virtual table with auto-sync triggers
nodes_fts(name, qualified_name, signature, docstring, file_path)
```

## MCP Tools Exposed

| Tool Name            | RPC Method | Description                       |
|----------------------|------------|-----------------------------------|
| codegraph_search     | Search     | FTS5 symbol search by name        |
| codegraph_context    | Context    | Symbol definition + callers/callees |
| codegraph_callers    | Callers    | BFS reverse traversal (call graph)|
| codegraph_callees    | Callees    | BFS forward traversal (call graph)|
| codegraph_impact     | Impact     | Blast radius analysis             |
| codegraph_node       | Node       | Single symbol details             |
| codegraph_status     | Status     | Index statistics                  |
| codegraph_files      | Files      | List indexed files                |

## Two Integration Modes

### Mode A: Direct stdio (Recommended for Claude Code)

```json
// .mcp.json in project root
{
  "mcpServers": {
    "codegraph": {
      "command": "/root/RPC_pro/codegraph-cpp/build/codegraph",
      "args": ["serve", "--mcp"],
      "env": {}
    }
  }
}
```

- Zero network overhead
- Single process, single user
- Claude Code spawns the process directly

### Mode B: TCP via mini-rpc (Multi-client / Remote)

```bash
# Terminal 1: Start RPC server
./codegraph_rpc_server_demo \
  --backend native \
  --db /root/RPC_pro/codegraph-cpp/.codegraph/index \
  --port 50051

# Terminal 2: Start MCP adapter (stdio)
./codegraph_mcp_adapter_demo --host 127.0.0.1 --port 50051
```

- Multiple MCP clients can share one server
- Supports remote deployment
- Health checks, load balancing, connection pooling
- `--backend native` directly reuses `codegraph-cpp`; `--backend sqlite` keeps schema-adapter compatibility
