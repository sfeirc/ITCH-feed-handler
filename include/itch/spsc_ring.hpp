#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace itch {

/// Lock-free Single-Producer Single-Consumer ring buffer.
///
/// Template parameters:
///   T  — element type (must be trivially copyable)
///   N  — capacity; MUST be a power of two
///
/// Layout:
///   head and tail are on separate cache lines to eliminate false sharing.
///   Storage is a plain std::array — zero heap allocation.
///
/// Ordering:
///   push() stores element, then does a release-store on tail so the
///   consumer sees a fully written element.
///   pop() does an acquire-load on tail (producer's publish fence) then
///   an acquire-load on head.
template<typename T, size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "SpscRing capacity N must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing element type must be trivially copyable");

    static constexpr size_t MASK = N - 1;

    // Producer-side cache line
    alignas(64) std::atomic<size_t> tail_{0};  ///< Written by producer, read by consumer
    char _pad0[64 - sizeof(std::atomic<size_t>)];

    // Consumer-side cache line
    alignas(64) std::atomic<size_t> head_{0};  ///< Written by consumer, read by producer
    char _pad1[64 - sizeof(std::atomic<size_t>)];

    // Storage — sized to avoid touching head/tail cache lines
    alignas(64) std::array<T, N> storage_{};

public:
    SpscRing() = default;

    // Non-copyable, non-movable — atomics are not movable
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /// Producer: attempt to enqueue one item.
    /// Returns true on success, false if the ring is full.
    [[nodiscard]] __attribute__((always_inline))
    bool push(const T& item) noexcept {
        const size_t cur_tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = (cur_tail + 1) & MASK;
        // Full when next_tail == head
        if (__builtin_expect(!!(next_tail == head_.load(std::memory_order_acquire)), 0)) {
            return false;
        }
        storage_[cur_tail] = item;
        // Release so consumer sees fully written storage_[cur_tail]
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    /// Consumer: attempt to dequeue one item.
    /// Returns the item on success, std::nullopt if the ring is empty.
    [[nodiscard]] __attribute__((always_inline))
    std::optional<T> pop() noexcept {
        const size_t cur_head = head_.load(std::memory_order_relaxed);
        // Acquire-load tail to synchronize with producer's release-store
        if (__builtin_expect(!!(cur_head == tail_.load(std::memory_order_acquire)), 0)) {
            return std::nullopt;
        }
        T item = storage_[cur_head];
        head_.store((cur_head + 1) & MASK, std::memory_order_release);
        return item;
    }

    /// Returns true when the ring contains no elements.
    /// Only safe to call from the consumer thread.
    [[nodiscard]] __attribute__((always_inline))
    bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_acquire);
    }

    /// Returns the number of elements currently enqueued.
    /// Approximate — both head and tail are sampled independently.
    [[nodiscard]] __attribute__((always_inline))
    size_t size_approx() const noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_relaxed);
        return (t - h + N) & MASK;
    }

    /// Capacity of the ring (one slot is always reserved as sentinel).
    static constexpr size_t capacity() noexcept { return N - 1; }
};

} // namespace itch
