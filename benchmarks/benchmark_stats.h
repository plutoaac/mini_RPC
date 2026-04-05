#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rpc::benchmark {

struct BenchmarkResult {
  std::string mode;
  int concurrency{0};
  int payload_bytes{0};
  int total_requests{0};
  int success_count{0};
  int failed_count{0};
  int timeout_count{0};
  double total_time_ms{0.0};
  double qps{0.0};
  double avg_latency_us{0.0};
  double p50_latency_us{0.0};
  double p95_latency_us{0.0};
  double p99_latency_us{0.0};
  long long max_latency_us{0};
};

struct LocalStats {
  std::vector<long long> latency_us;
  int success_count{0};
  int failed_count{0};
  int timeout_count{0};
};

class BenchmarkStats {
 public:
  BenchmarkStats(std::string mode, int concurrency, int payload_bytes,
                 int total_requests);

  void Merge(LocalStats local);

  BenchmarkResult Finalize(std::chrono::steady_clock::time_point begin,
                           std::chrono::steady_clock::time_point end);

 private:
  static double Percentile(const std::vector<long long>& sorted,
                           double quantile);

  std::string mode_;
  int concurrency_{0};
  int payload_bytes_{0};
  int total_requests_{0};

  std::vector<long long> latencies_us_;
  int success_count_{0};
  int failed_count_{0};
  int timeout_count_{0};
};

void PrintBenchmarkResult(const BenchmarkResult& result);
void PrintBenchmarkCsv(const BenchmarkResult& result);
std::string BenchmarkResultToText(const BenchmarkResult& result);
std::string BenchmarkResultToCsv(const BenchmarkResult& result);
bool WriteBenchmarkResultFiles(const BenchmarkResult& result,
                               std::string_view output_dir,
                               std::string_view file_stem,
                               std::string* text_path, std::string* csv_path);

}  // namespace rpc::benchmark
