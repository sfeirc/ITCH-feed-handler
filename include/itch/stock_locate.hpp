#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <optional>

namespace itch {

/// Maximum ITCH stock-locate value is 65535 (uint16_t).
/// ITCH 5.0 guarantees stock_locate fits in 2 bytes.
static constexpr size_t MAX_LOCATE = 65536;

/// Maximum length of an ITCH stock symbol (8 ASCII chars, space-padded).
static constexpr size_t SYMBOL_LEN = 8;

/// Flat O(1) bidirectional lookup between stock_locate (uint16_t) and symbol.
///
/// Backed by two arrays indexed directly by stock_locate value:
///   - locate_to_symbol_: stock_locate → 8-char symbol
///   - symbol_to_locate_: uses a small open-addressed hash map for symbol→locate
///
/// No heap allocation. Thread-compatible for reads after population is complete.
class StockLocate {
public:
    StockLocate() {
        // Zero-initialize
        for (auto& s : locate_to_symbol_) {
            s.fill('\0');
        }
        for (auto& e : sym_hash_) {
            e.locate = EMPTY_SLOT;
            e.sym.fill('\0');
        }
    }

    /// Register a mapping. Returns false if locate >= MAX_LOCATE.
    bool insert(uint16_t locate, const char* symbol, size_t sym_len) noexcept {
        if (locate == 0) return false;  // 0 is reserved in ITCH

        // Store locate → symbol
        auto& slot = locate_to_symbol_[locate];
        size_t copy_len = (sym_len < SYMBOL_LEN) ? sym_len : SYMBOL_LEN;
        std::memcpy(slot.data(), symbol, copy_len);

        // Store symbol → locate in hash table
        uint32_t h = hash_symbol(symbol, copy_len);
        for (size_t i = 0; i < SYM_HASH_CAP; ++i) {
            size_t idx = (h + i) & SYM_HASH_MASK;
            auto& entry = sym_hash_[idx];
            if (entry.locate == EMPTY_SLOT || entry.locate == locate) {
                entry.locate = locate;
                std::memcpy(entry.sym.data(), symbol, copy_len);
                return true;
            }
        }
        return false;  // Hash table full (shouldn't happen for valid ITCH data)
    }

    /// O(1) locate → symbol lookup. Returns "" if not found.
    [[nodiscard]] __attribute__((always_inline))
    std::string_view symbol(uint16_t locate) const noexcept {
        if (__builtin_expect(!!((locate == 0)), 0)) return {};
        const auto& s = locate_to_symbol_[locate];
        // Find trailing space
        size_t len = SYMBOL_LEN;
        while (len > 0 && s[len - 1] == ' ') --len;
        return {s.data(), len};
    }

    /// O(1) expected symbol → locate lookup. Returns 0 if not found.
    [[nodiscard]] __attribute__((always_inline))
    uint16_t locate(const char* sym, size_t sym_len) const noexcept {
        uint32_t h = hash_symbol(sym, sym_len);
        for (size_t i = 0; i < SYM_HASH_CAP; ++i) {
            size_t idx = (h + i) & SYM_HASH_MASK;
            const auto& entry = sym_hash_[idx];
            if (entry.locate == EMPTY_SLOT) return 0;
            if (std::memcmp(entry.sym.data(), sym, sym_len < SYMBOL_LEN ? sym_len : SYMBOL_LEN) == 0) {
                return entry.locate;
            }
        }
        return 0;
    }

private:
    static constexpr size_t SYM_HASH_CAP  = 1u << 17;  // 131072 slots
    static constexpr size_t SYM_HASH_MASK = SYM_HASH_CAP - 1;
    static constexpr uint16_t EMPTY_SLOT  = 0;  // locate 0 is reserved

    struct HashEntry {
        uint16_t locate;
        std::array<char, SYMBOL_LEN> sym;
    };

    // locate → symbol: direct indexed by locate value
    std::array<std::array<char, SYMBOL_LEN>, MAX_LOCATE> locate_to_symbol_;

    // symbol → locate: open-addressed hash map
    std::array<HashEntry, SYM_HASH_CAP> sym_hash_;

    static uint32_t hash_symbol(const char* s, size_t len) noexcept {
        // FNV-1a 32-bit
        uint32_t h = 2166136261u;
        size_t n = (len < SYMBOL_LEN) ? len : SYMBOL_LEN;
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<uint8_t>(s[i]);
            h *= 16777619u;
        }
        return h;
    }
};

} // namespace itch
