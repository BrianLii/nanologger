#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace nano::bench {

inline std::chrono::nanoseconds scheduled_offset(
    std::size_t sample, std::uint64_t rate, std::size_t batch_size) noexcept {
  if (rate == 0 || batch_size == 0) return {};
  const auto emitted = (sample / batch_size) * batch_size;
  const auto seconds = emitted / rate;
  const auto remainder = emitted % rate;
  return std::chrono::seconds(seconds) +
         std::chrono::nanoseconds(remainder * 1'000'000'000ULL / rate);
}

inline double calls_per_second(
    std::size_t calls, std::chrono::nanoseconds elapsed) noexcept {
  const double seconds = std::chrono::duration<double>(elapsed).count();
  return seconds > 0.0 ? static_cast<double>(calls) / seconds : 0.0;
}

template <typename Samples>
std::uint64_t nearest_rank(const Samples& sorted, double quantile) noexcept {
  if (sorted.empty()) return 0;
  if (quantile < 0.0) quantile = 0.0;
  if (quantile > 1.0) quantile = 1.0;
  auto rank = static_cast<std::size_t>(
      std::ceil(quantile * static_cast<double>(sorted.size())));
  if (rank == 0) rank = 1;
  return sorted[rank - 1];
}

}  // namespace nano::bench
