#include "net/pcap_replay.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace net {

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

PcapReplayer::~PcapReplayer() {
    if (buf_) {
        if (huge_page_) {
            ::munmap(buf_, buf_size_);
        } else {
            ::munmap(buf_, buf_size_);
        }
        buf_      = nullptr;
        buf_size_ = 0;
    }
}

// ---------------------------------------------------------------------------
// load_itch_binary()
// ---------------------------------------------------------------------------

bool PcapReplayer::load_itch_binary(const char* path) noexcept {
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    struct ::stat st{};
    if (::fstat(fd, &st) < 0) {
        ::close(fd);
        return false;
    }
    if (st.st_size <= 0) {
        ::close(fd);
        return false;
    }

    buf_size_ = static_cast<size_t>(st.st_size);

    // Try hugepage-backed mmap for optimal TLB coverage
    void* mapped = MAP_FAILED;

#ifdef MAP_HUGETLB
    mapped = ::mmap(nullptr, buf_size_,
                    PROT_READ,
                    MAP_PRIVATE | MAP_POPULATE | MAP_HUGETLB,
                    fd, 0);
    if (mapped != MAP_FAILED) {
        huge_page_ = true;
    }
#endif

    if (mapped == MAP_FAILED) {
        // Fall back to regular mmap
        mapped = ::mmap(nullptr, buf_size_,
                        PROT_READ,
                        MAP_PRIVATE | MAP_POPULATE,
                        fd, 0);
        huge_page_ = false;
    }

    ::close(fd);

    if (mapped == MAP_FAILED) return false;

    // Advise kernel to read-ahead sequentially
    ::madvise(mapped, buf_size_, MADV_SEQUENTIAL | MADV_WILLNEED);

    buf_ = mapped;
    scan_messages();
    return true;
}

// ---------------------------------------------------------------------------
// scan_messages() — count messages, validate format
// ---------------------------------------------------------------------------

void PcapReplayer::scan_messages() noexcept {
    const uint8_t* p   = static_cast<const uint8_t*>(buf_);
    const uint8_t* end = p + buf_size_;

    msg_count_  = 0;
    byte_count_ = 0;

    while (p + 2 <= end) {
        // 2-byte big-endian length of the ITCH message body
        uint16_t len;
        __builtin_memcpy(&len, p, 2);
        len = __builtin_bswap16(len);

        p += 2;
        if (len == 0) continue;
        if (p + len > end) break;  // truncated

        ++msg_count_;
        byte_count_ += len;
        p += len;
    }
}

// ---------------------------------------------------------------------------
// replay_all()
// ---------------------------------------------------------------------------

void PcapReplayer::replay_all(
    std::function<void(const uint8_t*, size_t)> cb) const noexcept
{
    if (!buf_) return;

    const uint8_t* p   = static_cast<const uint8_t*>(buf_);
    const uint8_t* end = p + buf_size_;

    while (p + 2 <= end) {
        uint16_t len;
        __builtin_memcpy(&len, p, 2);
        len = __builtin_bswap16(len);

        p += 2;
        if (len == 0) continue;
        if (p + len > end) break;

        cb(p, len);
        p += len;
    }
}

} // namespace net
