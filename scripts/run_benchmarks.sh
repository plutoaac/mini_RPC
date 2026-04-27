#!/bin/bash
#
# @file run_benchmarks.sh
# @brief 一键运行所有推荐 benchmark 并保存结果
#
# 用法：
#   ./scripts/run_benchmarks.sh [build_dir]
#
# 默认 build_dir 为 build
#
# 推荐使用 Release 构建以获得准确性能数据：
#   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-release -j
#   ./scripts/run_benchmarks.sh build-release
#
# Benchmark 默认使用 --log=off 关闭日志，以避免异步日志系统影响性能测量。
# 可使用 --log=info 做 A/B 对比，观察日志开销。
#
# 结果目录结构：
#   benchmarks/results/run_YYYYMMDD_HHMMSS/
#   ├── baseline/
#   ├── pipeline/
#   ├── conn_pool/
#   ├── thread_pool/
#   └── summary.md

set -euo pipefail

# 默认 build 目录
BUILD_DIR="${1:-build}"

# Benchmark 默认日志级别：off（避免日志影响 QPS 测量）
LOG_LEVEL="${LOG_LEVEL:-off}"

# 生成 timestamp 目录
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
RESULTS_BASE="benchmarks/results"
RESULTS_DIR="$RESULTS_BASE/run_$TIMESTAMP"

echo "=========================================="
echo "  mini_RPC Benchmark Suite"
echo "=========================================="
echo ""
echo "IMPORTANT: For accurate QPS measurement, use Release build:"
echo "  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build-release -j"
echo "  ./scripts/run_benchmarks.sh build-release"
echo ""
echo "Build directory: $BUILD_DIR"
echo "Log level: $LOG_LEVEL"
echo "Results directory: $RESULTS_DIR"
echo ""

# 可执行文件检查
check_executable() {
    local bin="$1"
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not found or not executable" >&2
        echo "Please run: cmake --build $BUILD_DIR -j" >&2
        return 1
    fi
    return 0
}

# 创建结果子目录
mkdir -p "$RESULTS_DIR/baseline"
mkdir -p "$RESULTS_DIR/pipeline"
mkdir -p "$RESULTS_DIR/conn_pool"
mkdir -p "$RESULTS_DIR/thread_pool"

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
echo ""

# =============================================
# B. Pipeline Benchmark (多 depth 扫描)
# =============================================
echo "=========================================="
echo "B. Pipeline Benchmark (depth sweep)"
echo "=========================================="
PIPELINE_BIN="$BUILD_DIR/rpc_benchmark_pipeline"
check_executable "$PIPELINE_BIN"

echo "--- depth=1 (serial baseline) ---"
"$PIPELINE_BIN" --requests=20000 --depth=1 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/pipeline"

echo ""
echo "--- depth=8 ---"
"$PIPELINE_BIN" --requests=50000 --depth=8 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/pipeline"

echo ""
echo "--- depth=32 ---"
"$PIPELINE_BIN" --requests=50000 --depth=32 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/pipeline"

echo ""
echo "--- depth=64 (higher concurrency) ---"
"$PIPELINE_BIN" --requests=50000 --depth=64 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/pipeline"

echo ""

# =============================================
# C. Connection Pool Benchmark (多连接 × 多 depth)
# =============================================
echo "=========================================="
echo "C. Connection Pool Benchmark (conns × depth)"
echo "=========================================="
CONN_POOL_BIN="$BUILD_DIR/rpc_benchmark_conn_pool"
check_executable "$CONN_POOL_BIN"

echo "--- conns=1, depth=8 (baseline) ---"
"$CONN_POOL_BIN" --requests=50000 --conns=1 --depth=8 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=4, depth=8 ---"
"$CONN_POOL_BIN" --requests=100000 --conns=4 --depth=8 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=4, depth=32 ---"
"$CONN_POOL_BIN" --requests=100000 --conns=4 --depth=32 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"

echo ""
echo "--- conns=8, depth=32 (recommended test point) ---"
"$CONN_POOL_BIN" --requests=100000 --conns=8 --depth=32 --payload_bytes=64 --log="$LOG_LEVEL" --output_dir="$RESULTS_DIR/conn_pool"

echo ""

# =============================================
# D. Thread Pool Benchmark (inline vs pool)
# =============================================
echo "=========================================="
echo "D. Thread Pool Benchmark (inline vs pool)"
echo "=========================================="
THREAD_POOL_BIN="$BUILD_DIR/rpc_thread_pool_benchmark"
check_executable "$THREAD_POOL_BIN"

echo "Running with handler_sleep_ms=20 (moderate slow handler)"
# Note: thread pool benchmark has its own port setup, log param via env
# 请求数 1024，并发 16，sleep 20ms
LOG_LEVEL="$LOG_LEVEL" "$THREAD_POOL_BIN" 1024 16 20 "$RESULTS_DIR/thread_pool"

echo ""

# =============================================
# E. Optional: Log overhead comparison
# =============================================
if [[ "${RUN_LOG_COMPARISON:-no}" == "yes" ]]; then
    echo "=========================================="
    echo "E. Log Overhead Comparison (optional)"
    echo "=========================================="
    echo ""
    echo "Running conn_pool with --log=info to measure logging overhead..."
    echo ""

    echo "--- with --log=info ---"
    "$CONN_POOL_BIN" --requests=50000 --conns=4 --depth=32 --payload_bytes=64 --log=info --output_dir="$RESULTS_DIR"

    echo ""
    echo "--- with --log=off ---"
    "$CONN_POOL_BIN" --requests=50000 --conns=4 --depth=32 --payload_bytes=64 --log=off --output_dir="$RESULTS_DIR"

    echo ""
    echo "Compare QPS results to understand logging overhead."
else
    echo "=========================================="
    echo "E. Log Overhead Comparison (skipped)"
    echo "=========================================="
    echo "To run log comparison: RUN_LOG_COMPARISON=yes ./scripts/run_benchmarks.sh"
    echo ""
fi

# =============================================
# 生成 summary
# =============================================
echo "=========================================="
echo "Generating summary..."
echo "=========================================="
python3 scripts/summarize_benchmarks.py "$RESULTS_DIR"

# =============================================
# 汇总
# =============================================
echo ""
echo "=========================================="
echo "Benchmark Complete!"
echo "=========================================="
echo ""
echo "Results saved to: $RESULTS_DIR"
echo ""
echo "Next steps:"
echo "  1. Check raw results:"
echo "     ls $RESULTS_DIR/baseline/"
echo "     ls $RESULTS_DIR/pipeline/"
echo "     ls $RESULTS_DIR/conn_pool/"
echo "     ls $RESULTS_DIR/thread_pool/"
echo "  2. View summary: cat $RESULTS_DIR/summary.md"
echo ""
echo "Interpretation:"
echo "  - QPS = success_count / elapsed_seconds"
echo "  - Only successful requests are counted"
echo "  - If failed_count > 0, interpret results carefully"
echo "  - Higher depth increases throughput but may increase tail latency"
echo "  - More connections can break single-connection bottleneck"
echo ""
echo "Note: If there are any failures/timeouts, check WARNING messages above."
echo "For accurate QPS, always use Release build and --log=off."
echo ""