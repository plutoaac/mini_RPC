#!/bin/bash
#
# @file run_benchmarks.sh
# @brief 一键运行所有推荐 benchmark 并保存结果
#
# 用法：
#   LOG_LEVEL=off ./scripts/run_benchmarks.sh build-release
#
# 默认 build_dir 为 build
# LOG_LEVEL 默认 off（避免日志影响 QPS 测量）
#
# 结果目录结构：
#   benchmarks/results/run_YYYYMMDD_HHMMSS/
#   ├── env.md               # 环境信息
#   ├── commands.md           # 运行命令记录
#   ├── baseline/
#   ├── pipeline/
#   ├── conn_pool/
#   ├── client_pool/
#   ├── thread_pool/
#   └── summary.md           # 自动生成

set -euo pipefail

BUILD_DIR="${1:-build}"
LOG_LEVEL="${LOG_LEVEL:-off}"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULTS_BASE="benchmarks/results"
RESULTS_DIR="$RESULTS_BASE/run_$TIMESTAMP"

echo "=========================================="
echo "  mini_RPC Benchmark Suite"
echo "=========================================="
echo ""
echo "Build directory: $BUILD_DIR"
echo "Log level: $LOG_LEVEL"
echo "Results directory: $RESULTS_DIR"
echo ""

# =============================================
# 创建目录 + 保存环境信息
# =============================================
mkdir -p "$RESULTS_DIR"/{baseline,pipeline,conn_pool,client_pool,thread_pool}

cat > "$RESULTS_DIR/env.md" << 'ENVEOF'
# Benchmark Environment

ENVEOF

{
  echo "## CPU"
  echo '```'
  lscpu | grep -E 'Model name|CPU\(s\)|Thread|Core|MHz|Socket' 2>/dev/null || true
  echo '```'
  echo ""
  echo "## Memory"
  echo '```'
  free -h 2>/dev/null || true
  echo '```'
  echo ""
  echo "## OS / Kernel"
  echo '```'
  uname -a 2>/dev/null || true
  echo '```'
  echo ""
  echo "## Compiler"
  echo '```'
  g++ --version 2>/dev/null | head -1 || true
  echo '```'
  echo ""
  echo "## Protobuf"
  echo '```'
  protoc --version 2>/dev/null || echo "protoc not found"
  echo '```'
  echo ""
  echo "## CMake Build Type"
  if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo '```'
    grep -E 'CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS_RELEASE|CMAKE_CXX_STANDARD' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null | grep -v ADVANCED || true
    echo '```'
  else
    echo "(CMakeCache.txt not found)"
  fi
  echo ""
  echo "## Benchmark Parameters"
  echo "- LOG_LEVEL: $LOG_LEVEL"
  echo "- Timestamp: $TIMESTAMP"
} >> "$RESULTS_DIR/env.md" 2>/dev/null

# 保存命令记录
CMDS_FILE="$RESULTS_DIR/commands.md"
cat > "$CMDS_FILE" << EOF
# Benchmark Commands

Run at: $(date -u '+%Y-%m-%d %H:%M:%S UTC')
Build dir: $BUILD_DIR
Log level: $LOG_LEVEL

EOF

record_cmd() {
  local section="$1"
  local cmd="$2"
  echo "## $section" >> "$CMDS_FILE"
  echo '```bash' >> "$CMDS_FILE"
  echo "$cmd" >> "$CMDS_FILE"
  echo '```' >> "$CMDS_FILE"
  echo "" >> "$CMDS_FILE"
}

# =============================================
# 可执行文件检查
# =============================================
check_executable() {
  local bin="$1"
  if [[ ! -x "$bin" ]]; then
    echo "ERROR: $bin not found or not executable" >&2
    echo "Please run: cmake --build $BUILD_DIR -j" >&2
    return 1
  fi
  return 0
}

echo "Results will be saved to: $RESULTS_DIR"
echo ""

# =============================================
# A. Baseline Benchmark
# =============================================
echo "=========================================="
echo "A. Baseline Benchmark (sync, 1000 requests)"
echo "=========================================="
BENCH_BIN="$BUILD_DIR/rpc_benchmark"
check_executable "$BENCH_BIN"
"$BENCH_BIN" --mode=sync --requests=1000 --concurrency=8 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/baseline"
record_cmd "Baseline Benchmark" "$BENCH_BIN --mode=sync --requests=1000 --concurrency=8 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/baseline"
echo ""

# =============================================
# B. Pipeline Benchmark
# =============================================
echo "=========================================="
echo "B. Pipeline Benchmark (depth sweep)"
echo "=========================================="
PIPELINE_BIN="$BUILD_DIR/rpc_benchmark_pipeline"
check_executable "$PIPELINE_BIN"

for depth in 1 8 32 64; do
  reqs=20000
  [ "$depth" -gt 1 ] && reqs=50000
  echo "--- depth=$depth, requests=$reqs ---"
  "$PIPELINE_BIN" --requests="$reqs" --depth="$depth" --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/pipeline"
  record_cmd "Pipeline depth=$depth" "$PIPELINE_BIN --requests=$reqs --depth=$depth --payload_bytes=64 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/pipeline"
  echo ""
done

# =============================================
# C. Connection Pool Benchmark
# =============================================
echo "=========================================="
echo "C. Connection Pool Benchmark (conns x depth)"
echo "=========================================="
CONN_POOL_BIN="$BUILD_DIR/rpc_benchmark_conn_pool"
check_executable "$CONN_POOL_BIN"

echo "--- conns=1, depth=8 (baseline) ---"
"$CONN_POOL_BIN" --requests=50000 --conns=1 --depth=8 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"
record_cmd "ConnPool conns=1 depth=8" "$CONN_POOL_BIN --requests=50000 --conns=1 --depth=8 --payload_bytes=64 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=4, depth=8 ---"
"$CONN_POOL_BIN" --requests=100000 --conns=4 --depth=8 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"
record_cmd "ConnPool conns=4 depth=8" "$CONN_POOL_BIN --requests=100000 --conns=4 --depth=8 --payload_bytes=64 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=4, depth=32 ---"
"$CONN_POOL_BIN" --requests=100000 --conns=4 --depth=32 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"
record_cmd "ConnPool conns=4 depth=32" "$CONN_POOL_BIN --requests=100000 --conns=4 --depth=32 --payload_bytes=64 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=8, depth=32 ---"
"$CONN_POOL_BIN" --requests=100000 --conns=8 --depth=32 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"
record_cmd "ConnPool conns=8 depth=32" "$CONN_POOL_BIN --requests=100000 --conns=8 --depth=32 --payload_bytes=64 --log=$LOG_LEVEL --output_dir=$RESULTS_DIR/conn_pool"

echo ""

# =============================================
# D. Thread Pool Benchmark (e2e RPC)
# =============================================
echo "=========================================="
echo "D. Thread Pool Benchmark (inline vs pool)"
echo "=========================================="
THREAD_POOL_BIN="$BUILD_DIR/rpc_thread_pool_benchmark"
check_executable "$THREAD_POOL_BIN"

echo "Running with handler_sleep_ms=20, requests=1024, concurrency=16"
LOG_LEVEL="$LOG_LEVEL" "$THREAD_POOL_BIN" 1024 16 20 "$RESULTS_DIR/thread_pool"
record_cmd "ThreadPool e2e" "$THREAD_POOL_BIN 1024 16 20 $RESULTS_DIR/thread_pool"

echo ""

# =============================================
# E. RpcClientPool Benchmark
# =============================================
echo "=========================================="
echo "E. RpcClientPool Benchmark (shared pool)"
echo "=========================================="
CLIENT_POOL_BIN="$BUILD_DIR/rpc_client_pool_benchmark"
check_executable "$CLIENT_POOL_BIN"

echo "--- scenario=round_robin, requests=50000, concurrency=8 ---"
"$CLIENT_POOL_BIN" --scenario=round_robin --requests=50000 --payload_bytes=64 --concurrency=8 --output_dir="$RESULTS_DIR/client_pool"
record_cmd "ClientPool round_robin 50k" "$CLIENT_POOL_BIN --scenario=round_robin --requests=50000 --payload_bytes=64 --concurrency=8 --output_dir=$RESULTS_DIR/client_pool"

echo ""
echo "--- scenario=least_inflight, requests=50000, concurrency=8 ---"
"$CLIENT_POOL_BIN" --scenario=least_inflight --requests=50000 --payload_bytes=64 --concurrency=8 --output_dir="$RESULTS_DIR/client_pool" || echo "WARNING: least_inflight failed"
record_cmd "ClientPool least_inflight 50k c=8" "$CLIENT_POOL_BIN --scenario=least_inflight --requests=50000 --payload_bytes=64 --concurrency=8 --output_dir=$RESULTS_DIR/client_pool"

echo ""
echo "--- scenario=round_robin, requests=100000, concurrency=16 ---"
"$CLIENT_POOL_BIN" --scenario=round_robin --requests=100000 --payload_bytes=64 --concurrency=16 --output_dir="$RESULTS_DIR/client_pool"
record_cmd "ClientPool round_robin 100k" "$CLIENT_POOL_BIN --scenario=round_robin --requests=100000 --payload_bytes=64 --concurrency=16 --output_dir=$RESULTS_DIR/client_pool"

echo ""

# =============================================
# F. ThreadPool Microbenchmark (before/after)
# =============================================
echo "=========================================="
echo "F. ThreadPool Microbenchmark (before/after)"
echo "=========================================="
TP_MICRO_BIN="$BUILD_DIR/thread_pool_benchmark"
if [ -x "$TP_MICRO_BIN" ]; then
  echo "--- tasks=20000, workers=4, producers=4 ---"
  "$TP_MICRO_BIN" 20000 4 4 > "$RESULTS_DIR/thread_pool/microbench_4w4p.txt" 2>&1
  cat "$RESULTS_DIR/thread_pool/microbench_4w4p.txt"
  record_cmd "ThreadPool micro 4w4p" "$TP_MICRO_BIN 20000 4 4"

  echo ""
  echo "--- tasks=50000, workers=4, producers=8 ---"
  "$TP_MICRO_BIN" 50000 4 8 > "$RESULTS_DIR/thread_pool/microbench_4w8p.txt" 2>&1
  cat "$RESULTS_DIR/thread_pool/microbench_4w8p.txt"
  record_cmd "ThreadPool micro 4w8p" "$TP_MICRO_BIN 50000 4 8"
else
  echo "WARNING: thread_pool_benchmark not found, skipping microbenchmark"
fi

echo ""

# =============================================
# 生成 summary
# =============================================
echo "=========================================="
echo "Generating summary..."
echo "=========================================="
python3 scripts/summarize_benchmarks.py "$RESULTS_DIR"

# =============================================
# 写 latest_summary.md 链接
# =============================================
LATEST="$RESULTS_BASE/latest_summary.md"
cat > "$LATEST" << EOF
# Latest Benchmark Summary

Latest run: [$TIMESTAMP](run_$TIMESTAMP/summary.md)

Full results: [run_$TIMESTAMP/](run_$TIMESTAMP/)
EOF

# =============================================
# 汇总
# =============================================
echo ""
echo "=========================================="
echo "Benchmark Complete!"
echo "=========================================="
echo ""
echo "Results saved to: $RESULTS_DIR"
echo "  Summary: $RESULTS_DIR/summary.md"
echo "  Env: $RESULTS_DIR/env.md"
echo "  Commands: $RESULTS_DIR/commands.md"
echo ""
echo "Next steps:"
echo "  cat $RESULTS_DIR/summary.md"
echo "  cat $RESULTS_DIR/env.md"
echo ""
