#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace itch {

/// Zero-copy view over a raw ITCH wire buffer.
/// All multi-byte integers in ITCH are big-endian; use read_be<T>() to decode.
struct MessageView {
    const uint8_t* data;  ///< Pointer into the receive buffer — not owned
    uint16_t       len;   ///< Total length of this message in bytes

    /// Returns the ITCH message type byte (first byte).
    [[nodiscard]] __attribute__((always_inline))
    uint8_t type() const noexcept { return data[0]; }

    /// Read a big-endian integer of type T starting at byte offset `off`.
    /// Uses memcpy-into-register to avoid UB from unaligned pointer casts.
    /// Supports uint8_t, uint16_t, uint32_t, uint64_t.
    template<typename T>
    [[nodiscard]] __attribute__((always_inline))
    T read_be(size_t off) const noexcept {
        static_assert(std::is_unsigned_v<T>, "read_be requires an unsigned integer type");
        T val;
        __builtin_memcpy(&val, data + off, sizeof(T));
        if constexpr (sizeof(T) == 1) { return val; }
        if constexpr (sizeof(T) == 2) { return static_cast<T>(__builtin_bswap16(val)); }
        if constexpr (sizeof(T) == 4) { return static_cast<T>(__builtin_bswap32(val)); }
        if constexpr (sizeof(T) == 8) { return static_cast<T>(__builtin_bswap64(val)); }
    }

    /// Read a 48-bit (6-byte) big-endian timestamp, returned as uint64_t nanoseconds.
    /// ITCH timestamps are nanoseconds since midnight UTC.
    [[nodiscard]] __attribute__((always_inline))
    uint64_t read_ts(size_t off) const noexcept {
        // Read 6 bytes big-endian as the high 6 bytes of a uint64
        uint64_t hi = 0;
        __builtin_memcpy(reinterpret_cast<uint8_t*>(&hi) + 2, data + off, 6);
        return __builtin_bswap64(hi);
    }

    /// Read a fixed-length ASCII field (e.g. 8-char stock symbol).
    /// Writes exactly `N` bytes into dst (no null terminator).
    template<size_t N>
    __attribute__((always_inline))
    void read_str(size_t off, char (&dst)[N]) const noexcept {
        __builtin_memcpy(dst, data + off, N);
    }
};

} // namespace itch
