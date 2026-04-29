#!/usr/bin/env python3
"""
@file summarize_benchmarks.py
@brief 读取 benchmark 结果并生成 Markdown summary

用法：
  python3 scripts/summarize_benchmarks.py <results_dir>

示例：
  python3 scripts/summarize_benchmarks.py benchmarks/results/run_20260426_220000

结果：
  在 results_dir 下生成 summary.md
"""

import csv
import sys
import os
from datetime import datetime
from typing import Dict, List, Optional


def read_csv(path: str) -> Optional[Dict[str, str]]:
    """读取 CSV 文件，返回第一行数据（作为字典）。"""
    try:
        with open(path, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            rows = list(reader)
            if rows:
                return rows[0]
            return None
    except Exception as e:
        print(f"WARNING: failed to read {path}: {e}", file=sys.stderr)
        return None


def check_fields(data: Dict[str, str], required: List[str]) -> bool:
    """检查数据是否包含所有必需字段。"""
    for field in required:
        if field not in data or data[field] is None or data[field] == '':
            return False
    return True


def safe_float(val: str, default: float = 0.0) -> float:
    """安全解析浮点数。"""
    try:
        return float(val)
    except (ValueError, TypeError):
        return default


def safe_int(val: str, default: int = 0) -> int:
    """安全解析整数。"""
    try:
        return int(float(val))
    except (ValueError, TypeError):
        return default


class TableBuilder:
    """生成 Markdown 表格。"""

    def __init__(self, headers: List[str]):
        self.headers = headers
        self.rows: List[List[str]] = []

    def add_row(self, values: List[str]):
        """添加一行数据。"""
        if len(values) != len(self.headers):
            print(f"WARNING: row has {len(values)} columns, expected {len(self.headers)}", file=sys.stderr)
            return
        self.rows.append(values)

    def render(self) -> str:
        """渲染为 Markdown 表格字符串。"""
        lines = []
        # 表头
        header_line = "| " + " | ".join(self.headers) + " |"
        separator_line = "| " + " | ".join(["---"] * len(self.headers)) + " |"
        lines.append(header_line)
        lines.append(separator_line)
        # 数据行
        for row in self.rows:
            line = "| " + " | ".join(str(v) for v in row) + " |"
            lines.append(line)
        return "\n".join(lines)


def parse_baseline(path: str) -> Optional[str]:
    """
    解析 baseline CSV。
    必需字段: mode, concurrency, payload_bytes, total_requests, success_count,
              failed_count, qps, avg_latency_us, p95_latency_us, p99_latency_us
    """
    data = read_csv(path)
    if not data:
        return None

    required = ['mode', 'concurrency', 'payload_bytes', 'total_requests',
                'success_count', 'failed_count', 'qps', 'avg_latency_us',
                'p95_latency_us', 'p99_latency_us']
    if not check_fields(data, required):
        print(f"WARNING: {path} missing required fields for baseline table, skipping", file=sys.stderr)
        return None

    builder = TableBuilder(['Mode', 'Concurrency', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'])
    builder.add_row([
        data['mode'],
        data['concurrency'],
        data['payload_bytes'],
        data['total_requests'],
        data['success_count'],
        data['failed_count'],
        f"{safe_float(data['qps']):.2f}",
        f"{safe_float(data['avg_latency_us']):.2f}",
        f"{safe_float(data['p95_latency_us']):.2f}",
        f"{safe_float(data['p99_latency_us']):.2f}"
    ])
    return builder.render()


def parse_pipeline(path: str) -> Optional[str]:
    """
    解析 pipeline CSV。
    必需字段: depth, total_requests, success, failed, qps, avg_us, p95_us, p99_us
    """
    data = read_csv(path)
    if not data:
        return None

    required = ['depth', 'total_requests', 'success', 'failed', 'qps', 'avg_us', 'p95_us', 'p99_us']
    if not check_fields(data, required):
        print(f"WARNING: {path} missing required fields for pipeline table, skipping", file=sys.stderr)
        return None

    builder = TableBuilder(['Depth', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'])
    builder.add_row([
        data['depth'],
        data['total_requests'],
        data['success'],
        data['failed'],
        f"{safe_float(data['qps']):.2f}",
        f"{safe_float(data['avg_us']):.2f}",
        f"{safe_float(data['p95_us']):.2f}",
        f"{safe_float(data['p99_us']):.2f}"
    ])
    return builder.render()


def parse_conn_pool(path: str) -> Optional[str]:
    """
    解析 conn_pool CSV。
    支持两种格式：
    1. WriteBenchmarkResultFiles 输出格式: mode,connections,depth,... (connections 字段)
    2. 原始 CSV 格式: conns,depth,total,ok,fail,timeout,... (conns 字段)
    """
    data = read_csv(path)
    if not data:
        return None

    # 检测格式：优先检查 connections 字段（WriteBenchmarkResultFiles 格式）
    if 'connections' in data:
        # 格式1: WriteBenchmarkResultFiles 格式 (mode,connections,depth,...)
        required = ['connections', 'depth', 'total_requests', 'success_count',
                    'failed_count', 'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
        if not check_fields(data, required):
            print(f"WARNING: {path} missing required fields for conn_pool format1, skipping", file=sys.stderr)
            return None
        builder = TableBuilder(['Connections', 'Depth', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'])
        builder.add_row([
            data['connections'],
            data['depth'],
            data.get('payload_bytes', data.get('payload', '64')),
            data['total_requests'],
            data['success_count'],
            data['failed_count'],
            f"{safe_float(data['qps']):.2f}",
            f"{safe_float(data['avg_latency_us']):.2f}",
            f"{safe_float(data['p95_latency_us']):.2f}",
            f"{safe_float(data['p99_latency_us']):.2f}"
        ])
    elif 'conns' in data:
        # 格式2: 原始 CSV 格式 (conns,depth,total,ok,fail,...)
        required = ['conns', 'depth', 'total', 'ok', 'fail', 'qps', 'avg_us', 'p95_us', 'p99_us']
        if not check_fields(data, required):
            print(f"WARNING: {path} missing required fields for conn_pool format2, skipping", file=sys.stderr)
            return None
        builder = TableBuilder(['Connections', 'Depth', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'])
        builder.add_row([
            data['conns'],
            data['depth'],
            data.get('payload', '64'),
            data['total'],
            data['ok'],
            data['fail'],
            f"{safe_float(data['qps']):.2f}",
            f"{safe_float(data['avg_us']):.2f}",
            f"{safe_float(data['p95_us']):.2f}",
            f"{safe_float(data['p99_us']):.2f}"
        ])
    else:
        print(f"WARNING: {path} no connections/conns field found, skipping", file=sys.stderr)
        return None
    
    return builder.render()


def parse_thread_pool(path: str) -> Optional[str]:
    """
    解析 thread_pool CSV。
    必需字段: mode, total_requests, success_count, failed_count, timeout_count,
              qps, avg_latency_us, p95_latency_us, p99_latency_us
    """
    data = read_csv(path)
    if not data:
        return None

    required = ['mode', 'total_requests', 'success_count', 'failed_count',
                'timeout_count', 'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
    if not check_fields(data, required):
        print(f"WARNING: {path} missing required fields for thread_pool table, skipping", file=sys.stderr)
        return None

    builder = TableBuilder(['Mode', 'Requests', 'Success', 'Failed', 'Timeout', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'])
    builder.add_row([
        data['mode'],
        data['total_requests'],
        data['success_count'],
        data['failed_count'],
        data['timeout_count'],
        f"{safe_float(data['qps']):.2f}",
        f"{safe_float(data['avg_latency_us']):.2f}",
        f"{safe_float(data['p95_latency_us']):.2f}",
        f"{safe_float(data['p99_latency_us']):.2f}"
    ])
    return builder.render()


def collect_csv_files(results_dir: str, subdir: str) -> List[str]:
    """收集子目录下的所有 CSV 文件。"""
    subdir_path = os.path.join(results_dir, subdir)
    if not os.path.isdir(subdir_path):
        return []
    csv_files = []
    for fname in os.listdir(subdir_path):
        if fname.endswith('.csv'):
            csv_files.append(os.path.join(subdir_path, fname))
    csv_files.sort()
    return csv_files


def render_table(headers: List[str], rows: List[List[str]]) -> str:
    """渲染 Markdown 表格（只渲染一次表头，固定列宽对齐）。"""
    if not rows:
        return ""
    
    # 计算每列最大宽度
    col_widths = [len(h) for h in headers]
    for row in rows:
        for i, val in enumerate(row):
            col_widths[i] = max(col_widths[i], len(str(val)))
    
    # 格式化行
    def format_row(values: List[str]) -> str:
        cells = []
        for i, v in enumerate(values):
            cells.append(str(v).ljust(col_widths[i]))
        return "| " + " | ".join(cells) + " |"
    
    lines = []
    # 表头
    lines.append(format_row(headers))
    # 分隔符
    separator = "| " + " | ".join("-" * w for w in col_widths) + " |"
    lines.append(separator)
    # 数据行
    for row in rows:
        lines.append(format_row(row))
    
    return "\n".join(lines) + "\n"


def generate_summary(results_dir: str) -> str:
    """生成完整的 Markdown summary。"""

    sections = []
    sections.append("# Benchmark Summary\n")
    sections.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")

    # 说明部分
    sections.append("## 如何理解结果\n")
    sections.append("**QPS (Queries Per Second)**: 成功请求的每秒吞吐量，`qps = success_count / elapsed_seconds`\n")
    sections.append("**Avg Latency**: 所有成功请求的平均端到端延迟（微秒）\n")
    sections.append("**P95/P99 Latency**: 95%/99% 请求的延迟不超过此值（微秒），反映尾延迟\n")
    sections.append("\n**重要提示**:\n")
    sections.append("- 只用成功请求计算 QPS\n")
    sections.append("- 如果 `failed_count` 或 `timeout_count` 不为 0，结果需要谨慎解释\n")
    sections.append("- `failed_count > 0` 时这组数据不能直接说明吞吐能力\n")
    sections.append("- `depth` 提高可能提升 QPS，但也可能提高 tail latency\n")
    sections.append("- `connections` 增加可能突破单连接瓶颈，但也会增加线程/连接成本\n\n")

    # Baseline Benchmark
    sections.append("## 1. Baseline Benchmark (rpc_benchmark)\n\n")
    sections.append("单连接基础延迟与吞吐测试，支持 sync/async/coroutine 三种调用模式。\n\n")
    csv_files = collect_csv_files(results_dir, "baseline")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            required = ['mode', 'concurrency', 'payload_bytes', 'total_requests',
                        'success_count', 'failed_count', 'qps', 'avg_latency_us',
                        'p95_latency_us', 'p99_latency_us']
            if not check_fields(data, required):
                continue
            rows.append([
                data['mode'],
                data['concurrency'],
                data['payload_bytes'],
                data['total_requests'],
                data['success_count'],
                data['failed_count'],
                f"{safe_float(data['qps']):.2f}",
                f"{safe_float(data['avg_latency_us']):.2f}",
                f"{safe_float(data['p95_latency_us']):.2f}",
                f"{safe_float(data['p99_latency_us']):.2f}"
            ])
        if rows:
            sections.append(render_table(
                ['Mode', 'Concurrency', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无数据*\n\n")

    # Pipeline Benchmark
    sections.append("## 2. Pipeline Benchmark (rpc_benchmark_pipeline)\n\n")
    sections.append("单连接多 in-flight 请求吞吐测试，观察 pipeline depth 对 QPS 和尾延迟的影响。\n\n")
    csv_files = collect_csv_files(results_dir, "pipeline")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            required = ['depth', 'total_requests', 'success', 'failed', 'qps', 'avg_us', 'p95_us', 'p99_us']
            if not check_fields(data, required):
                continue
            rows.append([
                data['depth'],
                data['total_requests'],
                data['success'],
                data['failed'],
                f"{safe_float(data['qps']):.2f}",
                f"{safe_float(data['avg_us']):.2f}",
                f"{safe_float(data['p95_us']):.2f}",
                f"{safe_float(data['p99_us']):.2f}"
            ])
        if rows:
            sections.append(render_table(
                ['Depth', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无数据*\n\n")

    # Connection Pool Benchmark
    sections.append("## 3. Connection Pool Benchmark (rpc_benchmark_conn_pool)\n\n")
    sections.append("多连接 × 每连接固定 in-flight 吞吐上限测试，观察连接数和深度对 QPS 的影响。\n\n")
    csv_files = collect_csv_files(results_dir, "conn_pool")
    if csv_files:
        rows = []
        for f in csv_files:
            table = parse_conn_pool(f)
            if table:
                # parse_conn_pool returns full table, extract rows from it
                # For simplicity, we re-parse directly
                data = read_csv(f)
                if not data:
                    continue
                conns = data.get('connections') or data.get('conns')
                if not conns:
                    continue
                # Check for WriteBenchmarkResultFiles format
                if 'connections' in data and 'total_requests' in data:
                    required = ['connections', 'depth', 'total_requests', 'success_count',
                                'failed_count', 'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
                    if not check_fields(data, required):
                        continue
                    rows.append([
                        data['connections'],
                        data['depth'],
                        data.get('payload_bytes', data.get('payload', '64')),
                        data['total_requests'],
                        data['success_count'],
                        data['failed_count'],
                        f"{safe_float(data['qps']):.2f}",
                        f"{safe_float(data['avg_latency_us']):.2f}",
                        f"{safe_float(data['p95_latency_us']):.2f}",
                        f"{safe_float(data['p99_latency_us']):.2f}"
                    ])
                elif 'total' in data and 'ok' in data:
                    # Original CSV format
                    required = ['conns', 'depth', 'total', 'ok', 'fail', 'qps', 'avg_us', 'p95_us', 'p99_us']
                    if not check_fields(data, required):
                        continue
                    rows.append([
                        data['conns'],
                        data['depth'],
                        data.get('payload', '64'),
                        data['total'],
                        data['ok'],
                        data['fail'],
                        f"{safe_float(data['qps']):.2f}",
                        f"{safe_float(data['avg_us']):.2f}",
                        f"{safe_float(data['p95_us']):.2f}",
                        f"{safe_float(data['p99_us']):.2f}"
                    ])
        if rows:
            sections.append(render_table(
                ['Connections', 'Depth', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无数据*\n\n")

    # Thread Pool Benchmark
    sections.append("## 4. Thread Pool Benchmark (rpc_thread_pool_benchmark)\n\n")
    sections.append("慢 handler 场景下 inline handler 与 business thread pool 的对比，观察线程池解耦效果。\n\n")
    csv_files = collect_csv_files(results_dir, "thread_pool")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            required = ['mode', 'total_requests', 'success_count', 'failed_count',
                        'timeout_count', 'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
            if not check_fields(data, required):
                continue
            rows.append([
                data['mode'],
                data['total_requests'],
                data['success_count'],
                data['failed_count'],
                data['timeout_count'],
                f"{safe_float(data['qps']):.2f}",
                f"{safe_float(data['avg_latency_us']):.2f}",
                f"{safe_float(data['p95_latency_us']):.2f}",
                f"{safe_float(data['p99_latency_us']):.2f}"
            ])
        if rows:
            sections.append(render_table(
                ['Mode', 'Requests', 'Success', 'Failed', 'Timeout', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无数据*\n\n")

    # RpcClientPool Benchmark
    sections.append("## 5. RpcClientPool Benchmark (rpc_client_pool_benchmark)\n\n")
    sections.append("共享连接池 + 负载均衡策略测试，观察不同分发策略对 QPS 和尾延迟的影响。\n\n")
    csv_files = collect_csv_files(results_dir, "client_pool")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            # 支持多种格式
            if 'scenario' in data:
                # 格式1: scenario,suc_count,fail_count,...
                required = ['scenario', 'suc_count', 'fail_count', 'total_time', 'avg_lat', 'p50_lat', 'p95_lat', 'p99_lat']
                if not check_fields(data, required):
                    continue
                total = safe_int(data['suc_count']) + safe_int(data['fail_count'])
                elapsed = safe_float(data['total_time']) / 1000.0  # ms to s
                qps = safe_int(data['suc_count']) / elapsed if elapsed > 0 else 0
                rows.append([
                    data['scenario'],
                    str(total),
                    data['suc_count'],
                    data['fail_count'],
                    f"{qps:.2f}",
                    f"{safe_float(data['avg_lat']):.2f}",
                    f"{safe_float(data['p95_lat']):.2f}",
                    f"{safe_float(data['p99_lat']):.2f}"
                ])
            elif 'concurrency' in data:
                # 格式2: concurrency,total_requests,...
                required = ['concurrency', 'total_requests', 'success_count', 'failed_count',
                            'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
                if not check_fields(data, required):
                    continue
                rows.append([
                    data.get('scenario', 'round_robin'),
                    data['total_requests'],
                    data['success_count'],
                    data['failed_count'],
                    f"{safe_float(data['qps']):.2f}",
                    f"{safe_float(data['avg_latency_us']):.2f}",
                    f"{safe_float(data['p95_latency_us']):.2f}",
                    f"{safe_float(data['p99_latency_us']):.2f}"
                ])
        if rows:
            sections.append(render_table(
                ['Scenario', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无数据*\n\n")

    # 快速参考
    sections.append("## 快速参考：每个 Benchmark 测什么\n\n")
    sections.append("| Benchmark | 测什么 | 关键参数 | 关注指标 |\n")
    sections.append("|-----------|--------|----------|----------|\n")
    sections.append("| rpc_benchmark | 基础延迟与吞吐 | mode (sync/async/co), concurrency | QPS, avg, p95 |\n")
    sections.append("| rpc_benchmark_pipeline | 单连接 pipeline 能力 | depth (in-flight 数量) | QPS 提升, p95/p99 上升 |\n")
    sections.append("| rpc_benchmark_conn_pool | 多连接吞吐上限 | conns, depth | QPS, tail latency |\n")
    sections.append("| rpc_client_pool_benchmark | 连接池负载均衡策略 | scenario (round_robin/least_inflight) | QPS, p95/p99 |\n")
    sections.append("| rpc_thread_pool_benchmark | 线程池解耦效果 | handler_sleep_ms | speedup ratio |\n")

    return "".join(sections)


def main():
    if len(sys.argv) < 2:
        # 默认找最新的 run 目录
        results_base = "benchmarks/results"
        if os.path.isdir(results_base):
            dirs = [d for d in os.listdir(results_base) 
                    if d.startswith("run_") and os.path.isdir(os.path.join(results_base, d))]
            dirs.sort(reverse=True)
            if dirs:
                results_dir = os.path.join(results_base, dirs[0])
                print(f"Using latest run directory: {results_dir}")
            else:
                print("ERROR: No run directories found in benchmarks/results", file=sys.stderr)
                print("Usage: python3 scripts/summarize_benchmarks.py <results_dir>")
                sys.exit(1)
        else:
            print("ERROR: benchmarks/results not found", file=sys.stderr)
            print("Usage: python3 scripts/summarize_benchmarks.py <results_dir>")
            sys.exit(1)
    else:
        results_dir = sys.argv[1]

    if not os.path.isdir(results_dir):
        print(f"ERROR: {results_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    summary_path = os.path.join(results_dir, "summary.md")
    summary_content = generate_summary(results_dir)

    with open(summary_path, 'w', encoding='utf-8') as f:
        f.write(summary_content)

    print(f"Summary written to: {summary_path}")
    print("\nPreview:")
    print("-" * 40)
    print(summary_content[:2000] + ("..." if len(summary_content) > 2000 else ""))


if __name__ == "__main__":
    main()