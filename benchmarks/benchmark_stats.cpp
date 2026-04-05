#include "benchmark_stats.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <utility>

namespace rpc::benchmark {

BenchmarkStats::BenchmarkStats(std::string mode, int concurrency,
                               int payload_bytes, int total_requests)
    : mode_(std::move(mode)),
      concurrency_(concurrency),
      payload_bytes_(payload_bytes),
      total_requests_(total_requests) {
  latencies_us_.reserve(static_cast<std::size_t>(std::max(0, total_requests_)));
}

void BenchmarkStats::Merge(LocalStats local) {
  success_count_ += local.success_count;
  failed_count_ += local.failed_count;
  timeout_count_ += local.timeout_count;

  if (!local.latency_us.empty()) {
    latencies_us_.insert(latencies_us_.end(), local.latency_us.begin(),
                         local.latency_us.end());
  }
}

BenchmarkResult BenchmarkStats::Finalize(
    const std::chrono::steady_clock::time_point begin,
    const std::chrono::steady_clock::time_point end) {
  std::sort(latencies_us_.begin(), latencies_us_.end());

  BenchmarkResult result;
  result.mode = mode_;
  result.concurrency = concurrency_;
  result.payload_bytes = payload_bytes_;
  result.total_requests = total_requests_;
  result.success_count = success_count_;
  result.failed_count = failed_count_;
  result.timeout_count = timeout_count_;

  const auto elapsed_us =
      std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();
  result.total_time_ms = static_cast<double>(elapsed_us) / 1000.0;
  if (elapsed_us > 0) {
    result.qps = (1e6 * static_cast<double>(result.success_count)) /
                 static_cast<double>(elapsed_us);
  }

  if (!latencies_us_.empty()) {
    const long long sum =
        std::accumulate(latencies_us_.begin(), latencies_us_.end(), 0LL);
    result.avg_latency_us =
        static_cast<double>(sum) / static_cast<double>(latencies_us_.size());
    result.p50_latency_us = Percentile(latencies_us_, 0.50);
    result.p95_latency_us = Percentile(latencies_us_, 0.95);
    result.p99_latency_us = Percentile(latencies_us_, 0.99);
  }

  return result;
}

double BenchmarkStats::Percentile(const std::vector<long long>& sorted,
                                  const double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }

  const auto clamped_q = std::clamp(quantile, 0.0, 1.0);
  const double raw_index = clamped_q * static_cast<double>(sorted.size() - 1);
  const std::size_t lo = static_cast<std::size_t>(std::floor(raw_index));
  const std::size_t hi = static_cast<std::size_t>(std::ceil(raw_index));
  const double frac = raw_index - static_cast<double>(lo);

  if (lo == hi) {
    return static_cast<double>(sorted[lo]);
  }

  return static_cast<double>(sorted[lo]) * (1.0 - frac) +
         static_cast<double>(sorted[hi]) * frac;
}

void PrintBenchmarkResult(const BenchmarkResult& result) {
  std::cout << BenchmarkResultToText(result);
}

void PrintBenchmarkCsv(const BenchmarkResult& result) {
  std::cout << "csv_summary=" << BenchmarkResultToCsv(result) << '\n';
}

std::string BenchmarkResultToText(const BenchmarkResult& result) {
  std::ostringstream oss;
  oss << "mode=" << result.mode << '\n';
  oss << "concurrency=" << result.concurrency << '\n';
  oss << "payload_bytes=" << result.payload_bytes << '\n';
  oss << "total_requests=" << result.total_requests << '\n';
  oss << "success_count=" << result.success_count << '\n';
  oss << "failed_count=" << result.failed_count << '\n';
  oss << "timeout_count=" << result.timeout_count << '\n';
  oss << "total_time_ms=" << result.total_time_ms << '\n';
  oss << "qps=" << result.qps << '\n';
  oss << "avg_latency_us=" << result.avg_latency_us << '\n';
  oss << "p50_latency_us=" << result.p50_latency_us << '\n';
  oss << "p95_latency_us=" << result.p95_latency_us << '\n';
  oss << "p99_latency_us=" << result.p99_latency_us << '\n';
  return oss.str();
}

std::string BenchmarkResultToCsv(const BenchmarkResult& result) {
  std::ostringstream oss;
  oss << result.mode << ',' << result.concurrency << ',' << result.payload_bytes
      << ',' << result.total_requests << ',' << result.success_count << ','
      << result.failed_count << ',' << result.timeout_count << ','
      << result.total_time_ms << ',' << result.qps << ','
      << result.avg_latency_us << ',' << result.p50_latency_us << ','
      << result.p95_latency_us << ',' << result.p99_latency_us;
  return oss.str();
}

bool WriteBenchmarkResultFiles(const BenchmarkResult& result,
                               const std::string_view output_dir,
                               const std::string_view file_stem,
                               std::string* text_path, std::string* csv_path) {
  namespace fs = std::filesystem;

  const fs::path base(output_dir);
  std::error_code ec;
  fs::create_directories(base, ec);
  if (ec) {
    return false;
  }

  const fs::path txt = base / (std::string(file_stem) + ".txt");
  const fs::path csv = base / (std::string(file_stem) + ".csv");

  {
    std::ofstream out(txt);
    if (!out.is_open()) {
      return false;
    }
    out << BenchmarkResultToText(result);
    out << "csv_summary=" << BenchmarkResultToCsv(result) << '\n';
  }

  {
    std::ofstream out(csv);
    if (!out.is_open()) {
      return false;
    }
    out << "mode,concurrency,payload_bytes,total_requests,success_count,failed_"
           "count,timeout_count,total_time_ms,qps,avg_latency_us,p50_latency_"
           "us,p95_latency_us,p99_latency_us\n";
    out << BenchmarkResultToCsv(result) << '\n';
  }

  if (text_path != nullptr) {
    *text_path = txt.string();
  }
  if (csv_path != nullptr) {
    *csv_path = csv.string();
  }
  return true;
}

}  // namespace rpc::benchmark
