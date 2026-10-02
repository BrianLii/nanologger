#include "nanologger.hpp"
#include "nanologger_binary.hpp"
#include "nanobench.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

static std::atomic<std::size_t> allocations{0};

void* operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(size)) return p;
  throw std::bad_alloc{};
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using Symbol = std::array<char, 8>;
using namespace std::chrono_literals;

namespace {

const Symbol symbol{'A', 'A', 'P', 'L', 0, 0, 0, 0};

struct TestFormatter {
  static void write(std::FILE* out, std::uint64_t ticks, std::uint64_t id,
                    const Symbol& symbol, double price, std::uint32_t qty) noexcept {
    std::fprintf(out, "%llu %llu %.8s %.2f %u\n",
                 static_cast<unsigned long long>(ticks),
                 static_cast<unsigned long long>(id), symbol.data(), price, qty);
  }
};

std::string read_line(std::FILE* file) {
  char line[128]{};
  return std::fgets(line, sizeof(line), file) ? line : "";
}

}  // namespace

TEST_CASE("bench pacing and throughput") {
  CHECK(nano::bench::scheduled_offset(0, 100'000, 1) == 0ns);
  CHECK(nano::bench::scheduled_offset(1, 100'000, 1) == 10us);
  CHECK(nano::bench::scheduled_offset(99, 100'000, 100) == 0ns);
  CHECK(nano::bench::scheduled_offset(100, 100'000, 100) == 1ms);
  CHECK(nano::bench::calls_per_second(100'000, 1s) == 100'000.0);
  CHECK(nano::bench::calls_per_second(100'000, 0ns) == 0.0);
}

TEST_CASE("bench nearest-rank percentiles") {
  std::vector<std::uint64_t> known(1000);
  for (std::size_t i = 0; i < known.size(); ++i) known[i] = i + 1;
  CHECK(nano::bench::nearest_rank(known, 0.50) == 500);
  CHECK(nano::bench::nearest_rank(known, 0.99) == 990);
  CHECK(nano::bench::nearest_rank(known, 0.999) == 999);
}

TEST_CASE("SpscRing is FIFO, bounded, and allocation-free on push") {
  std::FILE* file = std::tmpfile();
  REQUIRE(file != nullptr);
  nano::SpscRing<2, 64> ring;

  const auto before = allocations.load(std::memory_order_relaxed);
  const bool pushed = ring.try_push<TestFormatter>(
      11, std::uint64_t{7}, symbol, 123.45, std::uint32_t{9});
  const auto after = allocations.load(std::memory_order_relaxed);
  REQUIRE(pushed);
  CHECK(after == before);

  REQUIRE(ring.try_push<TestFormatter>(12, std::uint64_t{8}, symbol, 124.50,
                                       std::uint32_t{10}));
  CHECK_FALSE(ring.try_push<TestFormatter>(13, std::uint64_t{9}, symbol, 1.0,
                                           std::uint32_t{1}));
  CHECK(ring.try_consume(file));
  CHECK(ring.try_consume(file));
  CHECK_FALSE(ring.try_consume(file));

  std::rewind(file);
  CHECK(read_line(file) == "11 7 AAPL 123.45 9\n");
  CHECK(read_line(file) == "12 8 AAPL 124.50 10\n");
  std::fclose(file);
}

TEST_CASE("AsyncLogger drains accepted records on stop") {
  std::FILE* file = std::tmpfile();
  REQUIRE(file != nullptr);
  {
    nano::AsyncLogger<8, 64> logger(file);
    REQUIRE(logger.try_log<TestFormatter>(std::uint64_t{42}, symbol, 99.25,
                                          std::uint32_t{3}));
    logger.stop();
  }
  std::rewind(file);
  CHECK(read_line(file).find(" 42 AAPL 99.25 3\n") != std::string::npos);
  std::fclose(file);
}

namespace {

template <typename... Args>
constexpr bool binary_accepts(std::string_view format) {
  constexpr std::array<nano::detail::ArgType, sizeof...(Args)> types{
      nano::detail::arg_type<Args>()...};
  return nano::detail::format_matches(format, types);
}

static_assert(binary_accepts<std::uint64_t, Symbol, double>("%llu %.8s %.4f"));
static_assert(binary_accepts<>("100%% done"));
static_assert(!binary_accepts<std::uint64_t>("%u"));        // too narrow
static_assert(!binary_accepts<int>("%s"));                  // wrong class
static_assert(!binary_accepts<int>("%n"));                  // unsupported
static_assert(!binary_accepts<int>("%*d"));                 // consumes an extra arg
static_assert(!binary_accepts<int, int>("%d"));             // count mismatch
static_assert(!binary_accepts<int>("%d %"));                // dangling '%'

}  // namespace

TEST_CASE("Binary records decode to formatted text") {
  std::FILE* log = std::tmpfile();
  std::FILE* text = std::tmpfile();
  REQUIRE(log != nullptr);
  REQUIRE(text != nullptr);

  using Trade = nano::Binary<"Order %llu %.8s filled at %.4f, qty: %u">;
  using Mixed = nano::Binary<"%d%% of %hhu, x=%.2f">;
  using Started = nano::Binary<"started">;
  nano::SpscRing<8, 48> ring;
  REQUIRE(ring.try_push<Trade>(11, std::uint64_t{7}, symbol, 123.45,
                               std::uint32_t{9}));
  REQUIRE(ring.try_push<Mixed>(12, -5, std::uint8_t{200}, 1.5f));
  REQUIRE(ring.try_push<Started>(13));
  REQUIRE(ring.try_push<Trade>(14, std::uint64_t{8}, symbol, 124.5,
                               std::uint32_t{10}));
  // A fresh thread, because each consumer thread defines its sites once.
  std::thread([&] { while (ring.try_consume(log)) {} }).join();

  std::rewind(log);
  REQUIRE(nano::decode_binary(log, text));
  std::rewind(text);
  CHECK(read_line(text) == "11 Order 7 AAPL filled at 123.4500, qty: 9\n");
  CHECK(read_line(text) == "12 -5% of 200, x=1.50\n");
  CHECK(read_line(text) == "13 started\n");
  CHECK(read_line(text) == "14 Order 8 AAPL filled at 124.5000, qty: 10\n");
  CHECK(read_line(text).empty());

  std::FILE* unknown_site = std::tmpfile();
  REQUIRE(unknown_site != nullptr);
  const std::uint32_t id = 1;
  std::fwrite(&id, sizeof id, 1, unknown_site);
  std::rewind(unknown_site);
  CHECK_FALSE(nano::decode_binary(unknown_site, text));

  std::fclose(unknown_site);
  std::fclose(text);
  std::fclose(log);
}
