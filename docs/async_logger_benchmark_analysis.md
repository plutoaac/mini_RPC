# Async Logger Benchmark Analysis

## Benchmark Target

- Component: `src/common/async_logger.cpp`
- Queue: `src/common/mpsc_ring_queue.h` (MPSC, bounded, lock-free)
- Producer path: `rpc::common::LogInfo(...)`
- Consumer path: single background thread (`std::jthread`) + batch file write

## How To Run

Build target:

```bash
cd /root/RPC_pro/rpc_project/build
cmake --build . --target async_logger_benchmark -j
```

Run examples:

```bash
./async_logger_benchmark --threads=1 --messages-per-thread=400000 --message-bytes=128 --sample-stride=128 --log-file=./benchmarks/results/async_logger_bench_1t.log
./async_logger_benchmark --threads=8 --messages-per-thread=200000 --message-bytes=128 --sample-stride=256 --log-file=./benchmarks/results/async_logger_bench_8t.log
./async_logger_benchmark --threads=8 --messages-per-thread=200000 --message-bytes=128 --sample-stride=256 --log-file=/dev/null
```

## Added Runtime Counters

`LoggerRuntimeStats` now reports:

- `submit_calls`: total log submit calls
- `filtered_by_level`: dropped by level filter
- `enqueued`: successfully pushed into ring queue
- `consumed`: popped by consumer thread
- `dropped`: queue full drops (newest dropped)

This allows distinguishing:

- API submit throughput vs. effective enqueue throughput
- enqueue vs. consume lag
- real drop ratio under pressure

## Measured Results (Current Implementation)

### Scenario A: 1 thread, 400k, 128B payload, regular file

- submit throughput: ~1.44M msg/s
- enqueue throughput: ~567k msg/s
- consume throughput: ~509k msg/s
- dropped: 242,851 / 400,000
- drop rate: 60.7%

### Scenario B: 8 threads, 200k each, 128B payload, regular file

- submit throughput: ~4.43M msg/s
- enqueue throughput: ~340k msg/s
- consume throughput: ~295k msg/s
- dropped: 1,477,148 / 1,600,000
- drop rate: 92.3%

### Scenario C: 8 threads, 100k each, 128B payload, /dev/null

- submit throughput: ~4.52M msg/s
- enqueue throughput: ~420k msg/s
- consume throughput: ~329k msg/s
- dropped: 725,613 / 800,000
- drop rate: 90.7%

## Bottleneck Analysis

### 1) Single Consumer Capacity Is The Dominant Ceiling

Even in 1-thread producer mode, drop rate is already high (>60%).
This indicates the consumer side (batch formatting + write path) cannot sustain producer speed.

Key evidence:

- `submit_throughput` >> `enqueue_throughput`
- `enqueue_throughput` close to `consume_throughput`
- high drop appears before extreme producer contention

### 2) Producer Side Bursts Overflow Queue Quickly

Queue capacity is fixed (16,384). Under bursty multi-thread logging, producer aggregate rate overwhelms one consumer, and queue reaches full state quickly.

### 3) CAS Contention Exists But Is Not The First-Order Bottleneck

With 8 threads, producer CAS retries increase, but the strongest limitation still comes from consumer throughput ceiling.

### 4) Per-entry Dynamic String Cost Is Material

Each log call performs `entry.message.assign(...)`, which can allocate/copy frequently. This contributes noticeable producer overhead and cache churn.

## Optimization Roadmap

### P0 (Most Impactful, Low Risk)

1. Increase queue capacity and make it configurable
   - Example: 64K or 256K slots based on memory budget
   - Benefits: absorb bursts, lower short-window drop rate

2. Increase consumer batch size
   - Current: 256
   - Try: 512 / 1024
   - Benefits: reduce flush/check overhead per entry

3. Reduce formatting overhead in consumer
   - cache date/time prefix at second-level and append milliseconds fast path
   - precompute static prefix fragments where possible

### P1 (High Impact)

4. Introduce low-allocation message storage
   - replace per-entry `std::string` with fixed-size small buffer for common cases
   - fallback heap only for large messages

5. Improve file write strategy
   - evaluate `writev` or bigger coalesced buffer before `fwrite`
   - tune flush interval for benchmark profile

### P2 (Scalability)

6. Sharded async loggers
   - N queues + N consumer threads by hashing producer thread id
   - merge by time not required in most debug scenarios
   - expected near-linear throughput improvement on multi-core

7. Two-stage queue design
   - producer to per-thread SPSC queue
   - aggregator consumes SPSC queues to avoid heavy MPSC CAS hotspot

## Notes

- Current benchmark numbers are intentionally measured with drop-newest policy unchanged.
- Submit throughput alone is not enough; always inspect `enqueued/consumed/dropped`.
- For production SLO, track both drop rate and shutdown drain latency.

## Optimization Iterations (Latest)

Applied changes in `src/common/async_logger.cpp`:

- queue capacity: `1 << 18` (from earlier smaller values)
- batch size: 1024
- producer TID cached with `thread_local`
- reduced idle sleep and increased spin-before-sleep
- `setvbuf` 1MB buffer for file stream
- bounded producer retry before drop (`kPushRetryLimit = 2`)
- dropped-summary logs throttled and aggregated every 500ms

### Scenario D: 8 threads, 200k each, 128B payload, regular file (latest)

- submit throughput: ~2.21M msg/s
- enqueue throughput: ~859k msg/s
- consume throughput: ~498k msg/s
- dropped: 978,048 / 1,600,000
- drop rate: 61.1%

### Scenario E: 8 threads, 200k each, 128B payload, /dev/null (latest)

- submit throughput: ~1.80M msg/s
- enqueue throughput: ~697k msg/s
- consume throughput: ~403k msg/s
- dropped: 979,773 / 1,600,000
- drop rate: 61.2%

Compared with earlier ~92% drop under the same thread/message pressure, this round substantially improved effective enqueue rate and reduced drop ratio.

Trade-off observed:

- shutdown drain latency increased because queue is larger and can retain more backlog at stop time.
