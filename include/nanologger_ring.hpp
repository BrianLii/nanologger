#pragma once

// Single-producer/single-consumer ring of typed, deferred-format records.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace nano {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t cache_line =
    std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cache_line = 64;
#endif

template <std::size_t Capacity, std::size_t SlotBytes>
class SpscRing {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");

  // Published cursors are read by the remote side; each side's own position
  // and cached remote cursor live on a separate private line so the hot path
  // never loads a line the other core keeps pulling away.
  struct alignas(cache_line) Cursor {
    std::atomic<std::size_t> value{0};
  };

  struct alignas(cache_line) LocalCursor {
    std::size_t position{0};
    std::size_t observed_remote{0};
  };

  // Slots live in a 2 MiB-aligned block, rounded up to whole 2 MiB pages, so
  // Linux can back them with transparent huge pages: one TLB entry per 2 MiB
  // instead of one per 4 KiB page (64 slots).
  static constexpr std::size_t huge_page = std::size_t{2} << 20;

  template <typename Formatter, typename Tuple>
  static void decode(std::FILE* out, std::uint64_t ticks,
                     const void* bytes) noexcept {
    const auto* tuple =
        std::launder(reinterpret_cast<const Tuple*>(bytes));
    std::apply(
        [out, ticks](const auto&... args) noexcept {
          Formatter::write(out, ticks, args...);
        },
        *tuple);
  }

 public:
  ~SpscRing() { ::operator delete(slots_, std::align_val_t{huge_page}); }

  template <typename Formatter, typename... Args>
  bool try_push(std::uint64_t ticks, Args&&... args) noexcept {
    using Tuple = std::tuple<std::decay_t<Args>...>;
    static_assert((std::is_trivially_copyable_v<std::decay_t<Args>> && ...),
                  "hot-path arguments must be trivially copyable");
    static_assert(noexcept(Formatter::write(nullptr, 0, std::declval<const std::decay_t<Args>&>()...)));
    static_assert(std::is_nothrow_constructible_v<Tuple, Args&&...>);
    static_assert(std::is_trivially_destructible_v<Tuple>);
    static_assert(sizeof(Tuple) <= SlotBytes, "record exceeds slot payload");
    static_assert(alignof(Tuple) <= alignof(std::max_align_t));

    Slot* slot = claim();
    if (slot == nullptr) return false;

    std::construct_at(reinterpret_cast<Tuple*>(slot->payload.data()),
                      std::forward<Args>(args)...);
    slot->ticks = ticks;
    slot->decode = &decode<Formatter, Tuple>;
    publish();
    return true;
  }

  bool try_consume(std::FILE* out) noexcept {
    const auto tail = consumer_.position;
    auto head = consumer_.observed_remote;
    if (tail == head) {
      head = head_.value.load(std::memory_order_acquire);
      consumer_.observed_remote = head;
      if (tail == head) return false;
    }

    const Slot& slot = slots_[tail & (Capacity - 1)];
    slot.decode(out, slot.ticks, slot.payload.data());
    tail_.value.store(tail + 1, std::memory_order_release);
    consumer_.position = tail + 1;
    return true;
  }

  bool empty() const noexcept {
    return tail_.value.load(std::memory_order_acquire) ==
           head_.value.load(std::memory_order_acquire);
  }

 private:
  using Decode = void (*)(std::FILE*, std::uint64_t, const void*);

  // Line-aligned so a slot never shares a line with its neighbour; with the
  // 16-byte header, SlotBytes = 48 fills exactly one line.
  struct alignas(cache_line) Slot {
    Decode decode{};
    std::uint64_t ticks{};
    alignas(std::max_align_t) std::array<std::byte, SlotBytes> payload{};
  };

  // Returns the slot to fill, or nullptr when the ring is full. The record
  // becomes visible to the consumer only once publish() is called.
  Slot* claim() noexcept {
    const auto head = producer_.position;
    auto tail = producer_.observed_remote;
    if (head - tail >= Capacity) {
      tail = tail_.value.load(std::memory_order_acquire);
      producer_.observed_remote = tail;
      if (head - tail >= Capacity) return nullptr;
    }
    return &slots_[head & (Capacity - 1)];
  }

  static Slot* allocate_slots() {
    constexpr std::size_t bytes =
        (Capacity * sizeof(Slot) + huge_page - 1) / huge_page * huge_page;
    auto* slots = static_cast<Slot*>(
        ::operator new(bytes, std::align_val_t{huge_page}));
#if defined(__linux__)
    ::madvise(slots, bytes, MADV_HUGEPAGE);  // best effort; 4 KiB otherwise
#endif
    // Zeroing every slot also faults the pages in before the first push.
    std::uninitialized_value_construct_n(slots, Capacity);
    return slots;
  }

  void publish() noexcept {
    const auto head = producer_.position + 1;
    head_.value.store(head, std::memory_order_release);
    producer_.position = head;
  }

  Cursor head_;
  Cursor tail_;
  LocalCursor producer_;
  LocalCursor consumer_;
  Slot* const slots_ = allocate_slots();  // read-only after construction
};

}  // namespace nano
