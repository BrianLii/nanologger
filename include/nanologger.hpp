#pragma once

#include "nanologger_ring.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stop_token>
#include <thread>
#include <utility>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(__x86_64__)
#include <x86intrin.h>  // __rdtscp is not in <immintrin.h> on clang
#endif

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace nano {

#if defined(__x86_64__) || defined(_M_X64)
inline constexpr bool native_tsc = true;
inline std::uint64_t ticks_begin() noexcept {
  _mm_lfence();
  return __rdtsc();
}
inline std::uint64_t ticks_end() noexcept {
  unsigned int cpu = 0;
  const auto ticks = __rdtscp(&cpu);
  _mm_lfence();
  return ticks;
}
inline std::uint64_t ticks_now() noexcept { return __rdtsc(); }

#else
inline constexpr bool native_tsc = false;
inline std::uint64_t ticks_begin() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
inline std::uint64_t ticks_end() noexcept { return ticks_begin(); }
inline std::uint64_t ticks_now() noexcept { return ticks_begin(); }
#endif

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield");
#else
  std::this_thread::yield();
#endif
}

inline bool pin_this_thread(int cpu) noexcept {
  if (cpu < 0) return true;
#if defined(__linux__)
  if (cpu >= CPU_SETSIZE) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
  (void)cpu;
  return false;
#endif
}

template <std::size_t Capacity, std::size_t SlotBytes = 48>
class AsyncLogger {
 public:
  explicit AsyncLogger(std::FILE* output, int consumer_cpu = -1)
      : output_(output),
        worker_([this, consumer_cpu](std::stop_token stop) {
          run(stop, consumer_cpu);
        }) {}

  AsyncLogger(const AsyncLogger&) = delete;
  AsyncLogger& operator=(const AsyncLogger&) = delete;

  ~AsyncLogger() { stop(); }

  template <typename Formatter, typename... Args>
  bool try_log(Args&&... args) noexcept {
    return queue_.template try_push<Formatter>(
        ticks_now(), std::forward<Args>(args)...);
  }

  void flush() noexcept {
    while (!queue_.empty()) cpu_relax();
    std::fflush(output_);
  }

  void stop() noexcept {
    if (!worker_.joinable()) return;
    worker_.request_stop();
    worker_.join();
    std::fflush(output_);
  }

 private:
  void run(std::stop_token stop, int cpu) noexcept {
    pin_this_thread(cpu);
    while (!stop.stop_requested()) {
      if (!queue_.try_consume(output_)) cpu_relax();
    }
    while (queue_.try_consume(output_)) {}
  }

  SpscRing<Capacity, SlotBytes> queue_;
  std::FILE* output_;
  std::jthread worker_;
};

}  // namespace nano
