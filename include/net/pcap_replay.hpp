#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

namespace net {

/// ITCH binary replayer backed by a hugepage-aligned mmap buffer.
///
/// ITCH binary format (Nasdaq "ITCH binary" or "moldUDP64 payload"):
///   Each message is preceded by a 2-byte big-endian length field.
///   The message body immediately follows; the length does NOT include itself.
///
/// This class loads the entire file at startup so all replay I/O is from
/// page cache / huge-TLB, not from syscalls during the hot loop.
class PcapReplayer {
public:
    PcapReplayer() = default;
    ~PcapReplayer();

    PcapReplayer(const PcapReplayer&) = delete;
    PcapReplayer& operator=(const PcapReplayer&) = delete;

    /// Load an ITCH binary file.
    /// The buffer is mapped with MAP_HUGETLB | MAP_POPULATE where supported.
    /// Falls back to a regular mmap if huge pages are unavailable.
    /// Returns false on I/O error.
    [[nodiscard]] bool load_itch_binary(const char* path) noexcept;

    /// Replay all messages as fast as possible.
    /// cb is called with (message_body_ptr, body_len) for each message.
    /// The pointer is valid only for the duration of the callback.
    void replay_all(std::function<void(const uint8_t*, size_t)> cb) const noexcept;

    /// Number of ITCH messages found during load.
    [[nodiscard]] size_t message_count() const noexcept { return msg_count_; }

    /// Total bytes of ITCH message bodies (excluding length prefixes).
    [[nodiscard]] size_t byte_count() const noexcept { return byte_count_; }

    /// Raw buffer pointer — used by benchmarks to build pre-parsed corpora.
    [[nodiscard]] const uint8_t* raw_buf() const noexcept {
        return static_cast<const uint8_t*>(buf_);
    }
    [[nodiscard]] size_t raw_size() const noexcept { return buf_size_; }

    /// True if a file has been loaded successfully.
    [[nodiscard]] bool loaded() const noexcept { return buf_ != nullptr; }

private:
    void*  buf_       = nullptr;
    size_t buf_size_  = 0;
    size_t msg_count_ = 0;
    size_t byte_count_= 0;
    bool   huge_page_ = false;

    void scan_messages() noexcept;
};

} // namespace net
