#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

namespace net {

/// AF_XDP receiver with graceful fallback to recvmmsg + SO_BUSY_POLL.
///
/// Usage:
///   XdpSocket sock;
///   if (!sock.init("eth0", 21002)) { /* handle error */ }
///   while (running) { sock.recv_batch(process_packet); }
///   sock.close();
///
/// If AF_XDP is not available (kernel < 5.10, missing CAP_NET_ADMIN,
/// or libbpf not linked), init() returns true but transparently uses
/// a regular UDP socket with SO_BUSY_POLL=50.
class XdpSocket {
public:
    using PacketCallback = std::function<void(const uint8_t* data,
                                              size_t          len,
                                              uint64_t        hw_ts_ns)>;

    XdpSocket() = default;
    ~XdpSocket();

    XdpSocket(const XdpSocket&) = delete;
    XdpSocket& operator=(const XdpSocket&) = delete;

    /// Bind to the given interface and UDP port.
    /// Returns false on fatal error (e.g., port already in use).
    [[nodiscard]] bool init(const char* ifname, uint16_t port) noexcept;

    /// Receive up to kBatchSize packets and invoke cb for each.
    /// Non-blocking on the XDP path; uses SO_BUSY_POLL on the fallback path.
    /// Returns the number of packets processed.
    size_t recv_batch(const PacketCallback& cb) noexcept;

    /// Release all resources.
    void close() noexcept;

    /// True if AF_XDP path is active (false → fallback recvmmsg).
    [[nodiscard]] bool using_xdp() const noexcept { return use_xdp_; }

    /// True after successful init().
    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

private:
    static constexpr size_t   kBatchSize    = 512;
    static constexpr size_t   kUmemFrames   = 4096;
    static constexpr size_t   kFrameSize    = 2048;
    static constexpr size_t   kUmemSize     = kUmemFrames * kFrameSize;
    static constexpr int      kBusyPollUs   = 50;

    bool use_xdp_ = false;
    int  fd_      = -1;

    // ---- XDP path ----
    // When AF_XDP is available these hold the umem/ring state.
    // Defined as void* to keep this header free of libbpf types.
    // Guard with HAVE_LIBBPF so clang does not warn about unused private fields
    // when the library is not linked (-Wunused-private-field is an error here).
#ifdef HAVE_LIBBPF
    void* xsk_umem_   = nullptr;  // struct xsk_umem*
    void* xsk_socket_ = nullptr;  // struct xsk_socket*
    void* fill_ring_  = nullptr;  // struct xsk_ring_prod*
    void* rx_ring_    = nullptr;  // struct xsk_ring_cons*
#endif
    void* umem_buf_   = nullptr;  // mmap region (used by both XDP and fallback paths)

    // ---- Fallback path ----
    // Pre-allocated mmsghdr array for recvmmsg
    struct alignas(64) RecvBuf {
        uint8_t data[kFrameSize];
    };

    // We allocate these on the heap once at init time (not on hot path)
    RecvBuf*  recv_bufs_  = nullptr;
    void*     msg_hdrs_   = nullptr;  // mmsghdr[kBatchSize]
    void*     msg_iovecs_ = nullptr;  // iovec[kBatchSize]

    // ---- Helpers ----
    [[nodiscard]] bool init_xdp(const char* ifname, uint16_t port) noexcept;
    [[nodiscard]] bool init_fallback(const char* ifname, uint16_t port) noexcept;

    size_t recv_batch_xdp     (const PacketCallback& cb) noexcept;
    size_t recv_batch_fallback(const PacketCallback& cb) noexcept;

    static uint64_t hw_timestamp_ns(const void* cmsg_data) noexcept;
};

} // namespace net
