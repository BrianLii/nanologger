// Benchmark entry point: times each producer call of the binary logger. The
// producer stores typed arguments and the consumer writes Binary records.

#include "nanobench.hpp"
#include "nanologger.hpp"
#include "nanologger_binary.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Symbol = std::array<char, 8>;
using LatencySamples = std::vector<std::uint64_t>;

#ifndef NANOLOGGER_BENCH_QUEUE_SIZE
#define NANOLOGGER_BENCH_QUEUE_SIZE 32768
#endif
constexpr std::size_t kQueueSize = NANOLOGGER_BENCH_QUEUE_SIZE;
constexpr std::size_t kSlotPayloadBytes = 48;
constexpr std::size_t kTimerCalibrationSamples = 100'000;
constexpr std::size_t kWarmupSamples = 10'000;
constexpr std::uint64_t kRandomSeed = 0x9e3779b97f4a7c15ULL;
constexpr Symbol kSymbol{'A', 'A', 'P', 'L', 0, 0, 0, 0};

using Trade = nano::Binary<"Order %llu %.8s filled at %.4f, qty: %u">;
using Logger = nano::AsyncLogger<kQueueSize, kSlotPayloadBytes>;

struct Config {
  std::string mode{"steady"};
  std::string pacing{"spin"};
  std::string output{"/dev/null"};
  std::size_t samples{1'000'000};
  std::uint64_t rate{100'000};
  std::size_t burst_size{100};
  bool diagnostics{false};
  int producer_cpu{-1};
  int consumer_cpu{-1};
};

struct BenchmarkResult {
  explicit BenchmarkResult(std::size_t sample_count) {
    accepted_latency.reserve(sample_count);
    dropped_latency.reserve(sample_count);
  }

  LatencySamples accepted_latency;
  LatencySamples dropped_latency;
  std::chrono::nanoseconds elapsed{};
};

struct TimerCalibration {
  std::uint64_t floor;
  double ticks_per_ns;
};

[[noreturn]] void usage(const char* program) {
  std::fprintf(stderr,
               "usage: %s [--mode steady|burst] [--pacing sleep|spin] "
               "[--samples N] [--rate N] "
               "[--burst-size N] [--diagnostics 0|1] [--output PATH] "
               "[--producer-cpu N] [--consumer-cpu N]\n",
               program);
  std::exit(2);
}

Config parse_config(int argc, char** argv) {
  Config config;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) usage(argv[0]);
    if (std::strcmp(argv[i], "--mode") == 0)
      config.mode = argv[i + 1];
    else if (std::strcmp(argv[i], "--pacing") == 0)
      config.pacing = argv[i + 1];
    else if (std::strcmp(argv[i], "--samples") == 0)
      config.samples = std::strtoull(argv[i + 1], nullptr, 10);
    else if (std::strcmp(argv[i], "--rate") == 0)
      config.rate = std::strtoull(argv[i + 1], nullptr, 10);
    else if (std::strcmp(argv[i], "--burst-size") == 0)
      config.burst_size = std::strtoull(argv[i + 1], nullptr, 10);
    else if (std::strcmp(argv[i], "--diagnostics") == 0) {
      if (std::strcmp(argv[i + 1], "0") != 0 &&
          std::strcmp(argv[i + 1], "1") != 0)
        usage(argv[0]);
      config.diagnostics = std::strcmp(argv[i + 1], "1") == 0;
    }
    else if (std::strcmp(argv[i], "--output") == 0)
      config.output = argv[i + 1];
    else if (std::strcmp(argv[i], "--producer-cpu") == 0)
      config.producer_cpu = std::atoi(argv[i + 1]);
    else if (std::strcmp(argv[i], "--consumer-cpu") == 0)
      config.consumer_cpu = std::atoi(argv[i + 1]);
    else
      usage(argv[0]);
  }
  if ((config.mode != "steady" && config.mode != "burst") ||
      (config.pacing != "sleep" && config.pacing != "spin") ||
      config.samples == 0 || config.rate == 0 ||
      config.rate > 1'000'000'000 || config.burst_size == 0)
    usage(argv[0]);
  return config;
}

std::size_t workload_batch_size(const Config& config) {
  return config.mode == "burst" ? config.burst_size : 1;
}

std::uint64_t xorshift(std::uint64_t& state) noexcept {
  state ^= state << 13;
  state ^= state >> 7;
  state ^= state << 17;
  return state;
}

double calibrate_ticks_per_ns() {
  if (!nano::native_tsc) return 1.0;
  const auto wall_start = std::chrono::steady_clock::now();
  const auto tick_start = nano::ticks_begin();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto tick_end = nano::ticks_end();
  const auto wall_end = std::chrono::steady_clock::now();
  const double elapsed_ns =
      std::chrono::duration<double, std::nano>(wall_end - wall_start).count();
  return static_cast<double>(tick_end - tick_start) / elapsed_ns;
}

TimerCalibration calibrate_timer(std::size_t sample_count) {
  const auto calibration_count =
      std::min(sample_count, kTimerCalibrationSamples);
  LatencySamples overhead(calibration_count);
  for (auto& value : overhead) {
    const auto begin = nano::ticks_begin();
    value = nano::ticks_end() - begin;
  }
  std::sort(overhead.begin(), overhead.end());
  return {
      .floor = nano::bench::nearest_rank(overhead, 0.50),
      .ticks_per_ns = calibrate_ticks_per_ns(),
  };
}

double estimated_nanoseconds(std::uint64_t ticks,
                             const TimerCalibration& calibration) {
  const auto adjusted_ticks =
      ticks > calibration.floor ? ticks - calibration.floor : 0;
  return static_cast<double>(adjusted_ticks) / calibration.ticks_per_ns;
}

void warm_up(Logger& logger) {
  for (std::size_t i = 0; i < kWarmupSamples; ++i)
    while (!logger.try_log<Trade>(i, kSymbol, 100.0, std::uint32_t{1}))
      nano::cpu_relax();
  logger.flush();
}

// Paces the workload, times one producer call per sample, and separates
// accepted from dropped latency.
template <bool Spin>
void collect_latencies(const Config& config, Logger& logger,
                       BenchmarkResult& result) {
  std::uint64_t random_state = kRandomSeed;
  const auto workload_start =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  const auto batch_size = workload_batch_size(config);
  if constexpr (Spin) {
    while (std::chrono::steady_clock::now() < workload_start) nano::cpu_relax();
  } else {
    std::this_thread::sleep_until(workload_start);
  }
  for (std::size_t i = 0; i < config.samples; ++i) {
    const auto deadline = workload_start +
        nano::bench::scheduled_offset(i, config.rate, batch_size);
    if constexpr (Spin) {
      while (std::chrono::steady_clock::now() < deadline) nano::cpu_relax();
    } else {
      std::this_thread::sleep_until(deadline);
    }
    const auto random = xorshift(random_state);
    const double price =
        90.0 + static_cast<double>(random % 200'000) / 10'000.0;
    const auto qty = static_cast<std::uint32_t>(random % 1'000 + 1);
    const auto begin = nano::ticks_begin();
    const bool accepted = logger.try_log<Trade>(i + 1, kSymbol, price, qty);
    const auto elapsed = nano::ticks_end() - begin;
    (accepted ? result.accepted_latency : result.dropped_latency)
        .push_back(elapsed);
  }
  const auto workload_end =
      workload_start +
      nano::bench::scheduled_offset(config.samples, config.rate, 1);
  if constexpr (Spin) {
    while (std::chrono::steady_clock::now() < workload_end) nano::cpu_relax();
  } else {
    std::this_thread::sleep_until(workload_end);
  }
  result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - workload_start);
}

bool run_benchmark(const Config& config, BenchmarkResult& result) {
  std::FILE* output = std::fopen(config.output.c_str(), "wb");
  if (!output) {
    std::perror("fopen");
    return false;
  }

  auto logger = std::make_unique<Logger>(output, config.consumer_cpu);
  warm_up(*logger);
  if (config.pacing == "spin")
    collect_latencies<true>(config, *logger, result);
  else
    collect_latencies<false>(config, *logger, result);
  logger->stop();
  std::fclose(output);
  return true;
}

void report_latency(const char* label, LatencySamples& samples,
                    const TimerCalibration& calibration) {
  std::sort(samples.begin(), samples.end());
  const auto p50 = nano::bench::nearest_rank(samples, 0.50);
  const auto p99 = nano::bench::nearest_rank(samples, 0.99);
  const auto p999 = nano::bench::nearest_rank(samples, 0.999);
  std::printf("%s=%zu raw_ticks[p50=%llu p99=%llu p99.9=%llu] "
              "estimated_ns[p50=%.2f p99=%.2f p99.9=%.2f]\n",
              label, samples.size(), static_cast<unsigned long long>(p50),
              static_cast<unsigned long long>(p99),
              static_cast<unsigned long long>(p999),
              estimated_nanoseconds(p50, calibration),
              estimated_nanoseconds(p99, calibration),
              estimated_nanoseconds(p999, calibration));
}

void report_burst_diagnostics(const Config& config,
                              const BenchmarkResult& result) {
  if (!config.diagnostics || config.mode != "burst" ||
      !result.dropped_latency.empty())
    return;

  auto sorted = result.accepted_latency;
  std::sort(sorted.begin(), sorted.end());
  const auto p99 = nano::bench::nearest_rank(sorted, 0.99);
  std::array<std::size_t, 64> slow_by_slot_phase{};
  std::size_t slow_first = 0;
  std::size_t slow_other = 0;
  LatencySamples first;
  LatencySamples other;
  first.reserve(result.accepted_latency.size() / config.burst_size + 1);
  other.reserve(result.accepted_latency.size());
  for (std::size_t i = 0; i < result.accepted_latency.size(); ++i) {
    const auto ticks = result.accepted_latency[i];
    const bool is_first = i % config.burst_size == 0;
    (is_first ? first : other).push_back(ticks);
    if (ticks >= p99) {
      ++slow_by_slot_phase[(kWarmupSamples + i) % slow_by_slot_phase.size()];
      ++(is_first ? slow_first : slow_other);
    }
  }
  std::sort(first.begin(), first.end());
  std::sort(other.begin(), other.end());
  std::printf("diagnostics p99_cutoff_ticks=%llu first[p99=%llu slow=%zu/%zu] "
              "other[p99=%llu slow=%zu/%zu]",
              static_cast<unsigned long long>(p99),
              static_cast<unsigned long long>(
                  nano::bench::nearest_rank(first, 0.99)),
              slow_first, first.size(),
              static_cast<unsigned long long>(
                  nano::bench::nearest_rank(other, 0.99)),
              slow_other, other.size());
  for (std::size_t n = 0; n < 4; ++n) {
    const auto it = std::max_element(slow_by_slot_phase.begin(),
                                     slow_by_slot_phase.end());
    if (*it == 0) break;
    std::printf(" phase%zu=%zu",
                static_cast<std::size_t>(it - slow_by_slot_phase.begin()), *it);
    *it = 0;
  }
  std::putchar('\n');
}

void report(const Config& config, BenchmarkResult& result,
            const TimerCalibration& calibration) {
  std::printf("timer=%s ticks_per_ns=%.4f floor=%llu ticks\n",
              nano::native_tsc ? "x86-tsc" : "steady-clock",
              calibration.ticks_per_ns,
              static_cast<unsigned long long>(calibration.floor));
  std::printf("mode=%s pacing=%s samples=%zu rate=%llu/s burst_size=%zu queue_slots=%zu\n",
              config.mode.c_str(), config.pacing.c_str(), config.samples,
              static_cast<unsigned long long>(config.rate),
              workload_batch_size(config), kQueueSize);
  report_burst_diagnostics(config, result);
  report_latency("accepted", result.accepted_latency, calibration);
  report_latency("dropped", result.dropped_latency, calibration);
  const auto accepted = result.accepted_latency.size();
  std::printf("producer_throughput attempted=%.2f/s accepted=%.2f/s "
              "elapsed_ms=%.3f\n",
              nano::bench::calls_per_second(config.samples, result.elapsed),
              nano::bench::calls_per_second(accepted, result.elapsed),
              static_cast<double>(result.elapsed.count()) / 1'000'000.0);
  std::printf("accepted=%zu dropped=%zu\n", accepted,
              result.dropped_latency.size());
}

int report_validity(std::size_t dropped) {
  if (dropped == 0) {
    std::puts("baseline=VALID zero_drops=true");
    return 0;
  }

  std::fflush(stdout);
  std::fprintf(stderr, "baseline=INVALID reason=dropped_messages count=%zu\n",
               dropped);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  const Config config = parse_config(argc, argv);
  if (!nano::pin_this_thread(config.producer_cpu) && config.producer_cpu >= 0)
    std::fprintf(stderr, "warning: producer affinity is unsupported or failed\n");

  const TimerCalibration calibration = calibrate_timer(config.samples);
  BenchmarkResult result(config.samples);
  if (!run_benchmark(config, result)) return 1;

  report(config, result, calibration);
  return report_validity(result.dropped_latency.size());
}
