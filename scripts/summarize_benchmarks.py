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
import re
from datetime import datetime
from typing import Dict, List, Optional


def read_csv_all(path: str) -> Optional[List[Dict[str, str]]]:
    """读取 CSV 文件，返回所有行数据。"""
    try:
        with open(path, 'r', encoding='utf-8') as f:
            reader = csv.DictReader(f)
            return list(reader)
    except Exception as e:
        print(f"WARNING: failed to read {path}: {e}", file=sys.stderr)
        return None


def read_csv(path: str) -> Optional[Dict[str, str]]:
    """读取 CSV 文件，返回第一行数据。"""
    rows = read_csv_all(path)
    if rows:
        return rows[0]
    return None


def read_text(path: str) -> Optional[str]:
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return f.read()
    except Exception:
        return None


def check_fields(data: Dict[str, str], required: List[str]) -> bool:
    for field in required:
        if field not in data or data[field] is None or data[field] == '':
            return False
    return True


def safe_float(val: str, default: float = 0.0) -> float:
    try:
        return float(val)
    except (ValueError, TypeError):
        return default


def safe_int(val: str, default: int = 0) -> int:
    try:
        return int(float(val))
    except (ValueError, TypeError):
        return default


def render_table(headers: List[str], rows: List[List[str]]) -> str:
    if not rows:
        return "*无数据*\n\n"
    col_widths = [len(h) for h in headers]
    for row in rows:
        for i, val in enumerate(row):
            col_widths[i] = max(col_widths[i], len(str(val)))
    def format_row(values):
        return "| " + " | ".join(str(v).ljust(col_widths[i]) for i, v in enumerate(values)) + " |"
    lines = [format_row(headers)]
    lines.append("| " + " | ".join("-" * w for w in col_widths) + " |")
    for row in rows:
        lines.append(format_row(row))
    return "\n".join(lines) + "\n\n"


def collect_csv_files(results_dir: str, subdir: str) -> List[str]:
    subdir_path = os.path.join(results_dir, subdir)
    if not os.path.isdir(subdir_path):
        return []
    csv_files = [os.path.join(subdir_path, f) for f in os.listdir(subdir_path) if f.endswith('.csv')]
    csv_files.sort()
    return csv_files


def read_env_section(results_dir: str) -> str:
    """读取 env.md 并返回 Markdown 环境章节"""
    env_path = os.path.join(results_dir, "env.md")
    env_text = read_text(env_path)
    if env_text:
        return env_text + "\n"
    return ""


def parse_microbench_text(text: str) -> List[Dict]:
    """从 thread_pool microbenchmark 文本输出提取数据"""
    results = []
    current_name = None
    for line in text.split('\n'):
        m = re.match(r'^\[(.+)\]\s*$', line)
        if m:
            current_name = m.group(1).strip()
            continue
        if current_name and 'QPS=' in line:
            parts = {}
            for token in line.strip().split():
                if '=' in token:
                    k, v = token.split('=', 1)
                    try:
                        parts[k] = float(v)
                    except ValueError:
                        parts[k] = v
                elif token.endswith('x'):
                    continue  # SPEEDUP line
            if 'QPS' in parts:
                # Find latency line (next line after QPS)
                pass
            # Parse from format: "QPS=...  lat(us): avg=... p50=... p95=... p99=..."
            m2 = re.search(r'QPS=(\S+).*?avg=(\S+).*?p50=(\S+).*?p95=(\S+).*?p99=(\S+)', line)
            if m2:
                results.append({
                    'name': current_name,
                    'qps': safe_float(m2.group(1)),
                    'avg': safe_float(m2.group(2)),
                    'p50': safe_float(m2.group(3)),
                    'p95': safe_float(m2.group(4)),
                    'p99': safe_float(m2.group(5)),
                })
            current_name = None
    return results


def generate_summary(results_dir: str) -> str:
    sections = []

    # 标题
    sections.append("# RPC Framework Benchmark Summary\n")

    # 环境
    env_section = read_env_section(results_dir)
    if env_section:
        sections.append("## Environment\n")
        sections.append(env_section)

    # How to Reproduce
    sections.append("## How to Reproduce\n")
    sections.append("```bash\n")
    sections.append("# 1. Build (Release)\n")
    sections.append("cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release\n")
    sections.append("cmake --build build-release -j$(nproc)\n")
    sections.append("\n")
    sections.append("# 2. Run all benchmarks\n")
    sections.append("LOG_LEVEL=off ./scripts/run_benchmarks.sh build-release\n")
    sections.append("\n")
    sections.append("# 3. Generate summary (auto-called by script above)\n")
    sections.append("python3 scripts/summarize_benchmarks.py benchmarks/results/run_YYYYMMDD_HHMMSS\n")
    sections.append("```\n\n")

    # Interpretation guide
    sections.append("## Interpretation Guide\n\n")
    sections.append("- **QPS** = success_count / elapsed_seconds (仅成功请求)\n")
    sections.append("- **Avg(us)** = 所有成功请求的平均端到端延迟（微秒）\n")
    sections.append("- **P95(us) / P99(us)** = 95%/99% 请求的延迟不超过该值（微秒），反映尾延迟\n")
    sections.append("- **Failed** / **Timeout** 非零时该行数据**不可用于 QPS 对比**，需排查根本原因\n")
    sections.append("- `depth` 增大通常提升 QPS，但也可能拉高尾延迟\n")
    sections.append("- `connections` 增多可突破单连接瓶颈，但有线程/连接成本\n\n")

    # ============================================================
    # 1. Baseline Benchmark
    # ============================================================
    sections.append("## 1. Baseline Benchmark (`rpc_benchmark`)\n\n")
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
                print(f"WARNING: {f} missing baseline fields", file=sys.stderr)
                continue
            if safe_int(data['failed_count']) > 0:
                print(f"WARNING: {f} has {data['failed_count']} failures — QPS not trustworthy", file=sys.stderr)
            rows.append([
                data['mode'], data['concurrency'], data['payload_bytes'],
                data['total_requests'], data['success_count'], data['failed_count'],
                f"{safe_float(data['qps']):.1f}",
                f"{safe_float(data['avg_latency_us']):.1f}",
                f"{safe_float(data['p95_latency_us']):.1f}",
                f"{safe_float(data['p99_latency_us']):.1f}"
            ])
        if rows:
            sections.append(render_table(
                ['Mode', 'Concurrency', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无 CSV 文件*\n\n")

    # ============================================================
    # 2. Pipeline Benchmark
    # ============================================================
    sections.append("## 2. Pipeline Benchmark (`rpc_benchmark_pipeline`)\n\n")
    sections.append("单连接 + 多 in-flight 请求的吞吐测试，观察 pipeline depth 对 QPS 和尾延迟的影响。\n\n")
    csv_files = collect_csv_files(results_dir, "pipeline")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            required = ['depth', 'total_requests', 'success', 'failed', 'qps', 'avg_us', 'p95_us', 'p99_us']
            if not check_fields(data, required):
                print(f"WARNING: {f} missing pipeline fields", file=sys.stderr)
                continue
            if safe_int(data['failed']) > 0:
                print(f"WARNING: {f} has {data['failed']} failures", file=sys.stderr)
            rows.append([
                data['depth'], data['total_requests'], data['success'], data['failed'],
                f"{safe_float(data['qps']):.1f}",
                f"{safe_float(data['avg_us']):.1f}",
                f"{safe_float(data['p95_us']):.1f}",
                f"{safe_float(data['p99_us']):.1f}"
            ])
        if rows:
            sections.append(render_table(
                ['Depth', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无 CSV 文件*\n\n")

    # ============================================================
    # 3. Connection Pool Benchmark
    # ============================================================
    sections.append("## 3. Connection Pool Benchmark (`rpc_benchmark_conn_pool`)\n\n")
    sections.append("多连接 × 每连接固定 in-flight 吞吐上限测试。\n\n")
    csv_files = collect_csv_files(results_dir, "conn_pool")
    if csv_files:
        rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            conns = data.get('connections') or data.get('conns')
            if not conns:
                continue
            if 'connections' in data:
                required = ['connections', 'depth', 'total_requests', 'success_count',
                            'failed_count', 'qps', 'avg_latency_us', 'p95_latency_us', 'p99_latency_us']
                if not check_fields(data, required):
                    print(f"WARNING: {f} missing conn_pool fields (format1)", file=sys.stderr)
                    continue
                rows.append([
                    data['connections'], data['depth'],
                    data.get('payload_bytes', data.get('payload', '64')),
                    data['total_requests'], data['success_count'], data['failed_count'],
                    f"{safe_float(data['qps']):.1f}",
                    f"{safe_float(data['avg_latency_us']):.1f}",
                    f"{safe_float(data['p95_latency_us']):.1f}",
                    f"{safe_float(data['p99_latency_us']):.1f}"
                ])
            elif 'total' in data:
                required = ['conns', 'depth', 'total', 'ok', 'fail', 'qps', 'avg_us', 'p95_us', 'p99_us']
                if not check_fields(data, required):
                    print(f"WARNING: {f} missing conn_pool fields (format2)", file=sys.stderr)
                    continue
                rows.append([
                    data['conns'], data['depth'], data.get('payload', '64'),
                    data['total'], data['ok'], data['fail'],
                    f"{safe_float(data['qps']):.1f}",
                    f"{safe_float(data['avg_us']):.1f}",
                    f"{safe_float(data['p95_us']):.1f}",
                    f"{safe_float(data['p99_us']):.1f}"
                ])
        if rows:
            sections.append(render_table(
                ['Connections', 'Depth', 'Payload', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无 CSV 文件*\n\n")

    # ============================================================
    # 4. RpcClientPool Benchmark
    # ============================================================
    sections.append("## 4. RpcClientPool Benchmark (`rpc_client_pool_benchmark`)\n\n")
    sections.append("共享连接池 + 负载均衡策略测试（RoundRobin / LeastInflight）。\n\n")
    csv_files = collect_csv_files(results_dir, "client_pool")
    # 排除 endpoint stats CSV
    csv_files = [f for f in csv_files if '_endpoints.csv' not in f]
    if csv_files:
        rows = []
        endpoint_rows = []
        for f in csv_files:
            data = read_csv(f)
            if not data:
                continue
            # 新格式: scenario,concurrency,total_requests,payload_bytes,success_count,failed_count,qps,avg_us,p50_us,p95_us,p99_us
            if 'scenario' in data and 'p50_us' in data:
                required = ['scenario', 'total_requests', 'success_count', 'failed_count',
                            'qps', 'avg_us', 'p50_us', 'p95_us', 'p99_us']
                if not check_fields(data, required):
                    print(f"WARNING: {f} missing client_pool fields (new format)", file=sys.stderr)
                    continue
                rows.append([
                    data['scenario'],
                    data.get('concurrency', '?'),
                    data['total_requests'],
                    data['success_count'],
                    data['failed_count'],
                    f"{safe_float(data['qps']):.1f}",
                    f"{safe_float(data['avg_us']):.1f}",
                    f"{safe_float(data['p50_us']):.1f}",
                    f"{safe_float(data['p95_us']):.1f}",
                    f"{safe_float(data['p99_us']):.1f}"
                ])
            # 旧格式: scenario,suc_count,fail_count,total_time,...
            elif 'scenario' in data and 'total_time' in data:
                required = ['scenario', 'suc_count', 'fail_count', 'total_time', 'avg_lat', 'p95_lat', 'p99_lat']
                if not check_fields(data, required):
                    print(f"WARNING: {f} missing client_pool fields (old format)", file=sys.stderr)
                    continue
                total = safe_int(data['suc_count']) + safe_int(data['fail_count'])
                elapsed = safe_float(data['total_time']) / 1000.0
                qps = safe_int(data['suc_count']) / elapsed if elapsed > 0 else 0
                rows.append([
                    data['scenario'], '?', str(total), data['suc_count'], data['fail_count'],
                    f"{qps:.1f}",
                    f"{safe_float(data['avg_lat']):.1f}",
                    f"0.0",
                    f"{safe_float(data['p95_lat']):.1f}",
                    f"{safe_float(data['p99_lat']):.1f}"
                ])
            else:
                print(f"WARNING: {f} unknown client_pool CSV format", file=sys.stderr)

        # 解析 endpoint stats CSV（读取全部行）
        ep_csv_files = collect_csv_files(results_dir, "client_pool")
        ep_csv_files = [f for f in ep_csv_files if '_endpoints.csv' in f]
        for f in ep_csv_files:
            all_rows = read_csv_all(f)
            if not all_rows:
                continue
            for data in all_rows:
                if 'scenario' in data and 'endpoint' in data and 'select_count' in data:
                    endpoint_rows.append([
                        data.get('scenario', '?'),
                        data.get('endpoint', '?'),
                        data.get('select_count', '?'),
                        data.get('success_count', '?'),
                        data.get('fail_count', '?'),
                        data.get('healthy', '?')
                    ])

        if rows:
            sections.append(render_table(
                ['Scenario', 'Concurrency', 'Requests', 'Success', 'Failed', 'QPS', 'Avg(us)', 'P50(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")

        if endpoint_rows:
            sections.append("### Endpoint Stats\n\n")
            sections.append(render_table(
                ['Scenario', 'Endpoint', 'Select', 'Success', 'Fail', 'Healthy'],
                endpoint_rows
            ))
    else:
        sections.append("*无 CSV 文件*\n\n")

    # ============================================================
    # 5. ThreadPool End-to-End Benchmark
    # ============================================================
    sections.append("## 5. ThreadPool E2E Benchmark (`rpc_thread_pool_benchmark`)\n\n")
    sections.append("慢 handler 场景下 inline handler 与 business thread pool 的端到端对比。\n\n")
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
                print(f"WARNING: {f} missing thread_pool e2e fields", file=sys.stderr)
                continue
            if safe_int(data['failed_count']) > 0 or safe_int(data['timeout_count']) > 0:
                print(f"WARNING: {f} has failures/timeouts — results not trustworthy", file=sys.stderr)
            rows.append([
                data['mode'], data['total_requests'],
                data['success_count'], data['failed_count'], data['timeout_count'],
                f"{safe_float(data['qps']):.1f}",
                f"{safe_float(data['avg_latency_us']):.0f}",
                f"{safe_float(data['p95_latency_us']):.0f}",
                f"{safe_float(data['p99_latency_us']):.0f}"
            ])
        if rows:
            sections.append(render_table(
                ['Mode', 'Requests', 'Success', 'Failed', 'Timeout', 'QPS', 'Avg(us)', 'P95(us)', 'P99(us)'],
                rows
            ))
        else:
            sections.append("*无有效数据*\n\n")
    else:
        sections.append("*无 CSV 文件*\n\n")

    # ============================================================
    # 6. ThreadPool Microbenchmark Summary
    # ============================================================
    sections.append("## 6. ThreadPool Microbenchmark (`thread_pool_benchmark`)\n\n")
    sections.append("纯 ThreadPool 架构对比：旧版 (mutex+deque) vs 新版 (per-worker MpscRingQueue)。")
    sections.append("注意：这是 microbenchmark，不等同于完整 RPC 性能。\n\n")

    for txt_file in sorted([f for f in os.listdir(os.path.join(results_dir, "thread_pool"))
                             if f.endswith('.txt')]):
        txt_path = os.path.join(results_dir, "thread_pool", txt_file)
        txt = read_text(txt_path)
        if not txt:
            continue

        result_name = txt_file.replace('.txt', '').replace('microbench_', '')
        sections.append(f"### {result_name}\n\n")

        # Parse the text output
        in_table = False
        table_rows = []
        section_name = ""
        for line in txt.split('\n'):
            if line.startswith('--- Scenario:'):
                section_name = line.replace('--- Scenario:', '').strip().rstrip('-').strip()
            if line.startswith('[OLD'):
                old_line = line
            if line.startswith('[NEW'):
                new_line = line
            if 'SPEEDUP:' in line:
                speedup = line.split('SPEEDUP:')[1].strip()
                # Parse OLD and NEW lines
                old_match = re.search(r'QPS=(\S+)\s+.*?avg=(\S+)\s+p50=(\S+)\s+p95=(\S+)\s+p99=(\S+)', old_line) if 'old_line' in dir() else None
                new_match = re.search(r'QPS=(\S+)\s+.*?avg=(\S+)\s+p50=(\S+)\s+p95=(\S+)\s+p99=(\S+)', new_line) if 'new_line' in dir() else None
                if old_match and new_match:
                    table_rows.append([section_name, 'OLD',
                        f"{safe_float(old_match.group(1)):.0f}",
                        f"{safe_float(old_match.group(3)):.0f}",
                        f"{safe_float(old_match.group(4)):.0f}",
                        f"{safe_float(old_match.group(5)):.0f}",
                        f"{safe_float(old_match.group(2)):.0f}"])
                    table_rows.append([section_name, 'NEW',
                        f"{safe_float(new_match.group(1)):.0f}",
                        f"{safe_float(new_match.group(3)):.0f}",
                        f"{safe_float(new_match.group(4)):.0f}",
                        f"{safe_float(new_match.group(5)):.0f}",
                        f"{safe_float(new_match.group(2)):.0f}", speedup])

        if table_rows:
            # Simple column format: Scenario | Ver | QPS | p50(us) | p95(us) | p99(us) | Avg(us) | Speedup
            display_rows = []
            for r in table_rows:
                display_rows.append([r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7] if len(r) > 7 else ''])
            sections.append(render_table(
                ['Scenario', 'Ver', 'QPS', 'p50(us)', 'p95(us)', 'p99(us)', 'Avg(us)', 'Speedup'],
                display_rows
            ))

    # ============================================================
    # Caveats
    # ============================================================
    sections.append("## Caveats\n\n")
    sections.append("1. **Microbenchmark != E2E**: `thread_pool_benchmark` 测的是纯 ThreadPool 调度性能，")
    sections.append("不代表 RPC 端到端吞吐。E2E 结果见 Section 5。\n")
    sections.append("2. **QPS 下降但 p50 改善**：新版有界队列满时阻塞生产者（而非丢弃），")
    sections.append("导致 QPS 指标包含生产者等待时间。**延迟指标更真实反映调度性能**。\n")
    sections.append("3. **Release 模式下的 assert()**: `NDEBUG` 使 `assert(Register(...))` 跳过，")
    sections.append("某些 e2e benchmark 在 Release 下可能因 service 未注册而失败。\n")
    sections.append("4. **单机测试局限性**：client 和 server 共享 CPU/内存，测的不是纯网络性能。\n")
    sections.append("5. **队列容量限制**：MpscRingQueue 定容 1024/worker，极端突发下生产者阻塞，" )
    sections.append("可通过增大 `kQueueCapacity` 或按需动态调整。\n")

    sections.append(f"\n---\n*Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}*\n")
    return "".join(sections)


def main():
    results_base = "benchmarks/results"
    if len(sys.argv) < 2:
        if os.path.isdir(results_base):
            dirs = [d for d in os.listdir(results_base)
                    if d.startswith("run_") and os.path.isdir(os.path.join(results_base, d))]
            dirs.sort(reverse=True)
            if dirs:
                results_dir = os.path.join(results_base, dirs[0])
                print(f"Using latest run directory: {results_dir}")
            else:
                print("ERROR: No run directories found", file=sys.stderr)
                sys.exit(1)
        else:
            print("ERROR: benchmarks/results not found", file=sys.stderr)
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
    print(f"  Length: {len(summary_content)} chars")


if __name__ == "__main__":
    main()
