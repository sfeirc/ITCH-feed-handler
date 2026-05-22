#include "net/xdp_socket.hpp"

// System headers
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
#include <linux/net_tstamp.h>
#include <sys/time.h>
#include <time.h>

// recvmmsg
#include <sys/types.h>

// Conditionally include libbpf/xsk.h for XDP support
#ifdef HAVE_LIBBPF
#  include <bpf/xsk.h>
#  include <bpf/libbpf.h>
#endif

namespace net {

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

XdpSocket::~XdpSocket() {
    close();
}

// ---------------------------------------------------------------------------
// init() — try XDP, fall back to recvmmsg
// ---------------------------------------------------------------------------

bool XdpSocket::init(const char* ifname, uint16_t port) noexcept {
    // Try XDP path first (requires root + libbpf + supported kernel)
#ifdef HAVE_LIBBPF
    if (init_xdp(ifname, port)) {
        use_xdp_ = true;
        return true;
    }
#endif
    // Fallback: regular UDP socket
    return init_fallback(ifname, port);
}

// ---------------------------------------------------------------------------
// XDP path
// ---------------------------------------------------------------------------

#ifdef HAVE_LIBBPF
bool XdpSocket::init_xdp(const char* ifname, uint16_t port) noexcept {
    const unsigned int ifindex = ::if_nametoindex(ifname);
    if (ifindex == 0) return false;

    // Allocate UMEM buffer
    void* umem_buf = nullptr;
    if (::posix_memalign(&umem_buf, getpagesize(),
                         kUmemSize) != 0) return false;
    umem_buf_ = umem_buf;

    // Create UMEM
    struct xsk_umem_config umem_cfg{};
    umem_cfg.fill_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size     = kFrameSize;
    umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;

    struct xsk_umem*       umem = nullptr;
    struct xsk_ring_prod*  fill = new struct xsk_ring_prod{};
    struct xsk_ring_cons*  comp = new struct xsk_ring_cons{};

    if (::xsk_umem__create(&umem, umem_buf, kUmemSize,
                            fill, comp, &umem_cfg) != 0) {
        delete fill; delete comp;
        ::free(umem_buf); umem_buf_ = nullptr;
        return false;
    }
    xsk_umem_ = umem;
    fill_ring_ = fill;

    // Pre-populate fill ring
    uint32_t idx_fq = 0;
    const uint32_t reserved = ::xsk_ring_prod__reserve(fill, kUmemFrames, &idx_fq);
    for (uint32_t i = 0; i < reserved; ++i) {
        *::xsk_ring_prod__fill_addr(fill, idx_fq++) = static_cast<uint64_t>(i) * kFrameSize;
    }
    ::xsk_ring_prod__submit(fill, reserved);

    // Create XDP socket
    struct xsk_socket_config xsk_cfg{};
    xsk_cfg.rx_size        = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size        = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.libbpf_flags   = 0;
    xsk_cfg.xdp_flags      = XDP_FLAGS_DRV_MODE;
    xsk_cfg.bind_flags     = XDP_USE_NEED_WAKEUP;

    struct xsk_socket* xsk = nullptr;
    struct xsk_ring_cons* rx = new struct xsk_ring_cons{};

    if (::xsk_socket__create(&xsk, ifname, 0, umem,
                              rx, nullptr, &xsk_cfg) != 0) {
        delete rx; delete fill; delete comp;
        ::xsk_umem__delete(umem);
        ::free(umem_buf); umem_buf_ = nullptr;
        return false;
    }

    xsk_socket_ = xsk;
    rx_ring_    = rx;
    fd_         = ::xsk_socket__fd(xsk);
    (void)port;  // BPF program filters by port
    return true;
}
#else
bool XdpSocket::init_xdp(const char* /*ifname*/, uint16_t /*port*/) noexcept {
    return false;  // libbpf not available
}
#endif  // HAVE_LIBBPF

// ---------------------------------------------------------------------------
// Fallback: UDP socket with SO_BUSY_POLL + recvmmsg + hardware timestamps
// ---------------------------------------------------------------------------

bool XdpSocket::init_fallback(const char* ifname, uint16_t port) noexcept {
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    if (fd_ < 0) return false;

    // Bind to all interfaces on the given port
    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    int opt = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_); fd_ = -1;
        return false;
    }

    // Request hardware timestamps
    int ts_flags = SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_RAW_HARDWARE |
                   SOF_TIMESTAMPING_RX_SOFTWARE;
    ::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags, sizeof(ts_flags));

    // SO_BUSY_POLL: spin in kernel for up to 50µs before sleeping
    int busy_poll = kBusyPollUs;
    ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

    // Increase receive buffer
    int rcvbuf = 16 * 1024 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Bind to interface if specified (for multicast)
    if (ifname && ifname[0] != '\0') {
        ::setsockopt(fd_, SOL_SOCKET, SO_BINDTODEVICE, ifname,
                     static_cast<socklen_t>(::strlen(ifname) + 1));
    }

    // Allocate receive buffers and msg headers (one-time heap alloc at init)
    recv_bufs_   = new RecvBuf[kBatchSize];
    msg_hdrs_    = ::calloc(kBatchSize, sizeof(struct mmsghdr));
    msg_iovecs_  = ::calloc(kBatchSize, sizeof(struct iovec));

    auto* hdrs   = static_cast<struct mmsghdr*>(msg_hdrs_);
    auto* iovecs = static_cast<struct iovec*>(msg_iovecs_);

    for (size_t i = 0; i < kBatchSize; ++i) {
        iovecs[i].iov_base = recv_bufs_[i].data;
        iovecs[i].iov_len  = sizeof(RecvBuf);
        hdrs[i].msg_hdr.msg_iov    = &iovecs[i];
        hdrs[i].msg_hdr.msg_iovlen = 1;
        // No ancillary data setup for speed (timestamps via cmsg omitted in batch path)
    }

    use_xdp_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// recv_batch()
// ---------------------------------------------------------------------------

size_t XdpSocket::recv_batch(const PacketCallback& cb) noexcept {
#ifdef HAVE_LIBBPF
    if (use_xdp_) return recv_batch_xdp(cb);
#endif
    return recv_batch_fallback(cb);
}

#ifdef HAVE_LIBBPF
size_t XdpSocket::recv_batch_xdp(const PacketCallback& cb) noexcept {
    auto* rx   = static_cast<struct xsk_ring_cons*>(rx_ring_);
    auto* fill = static_cast<struct xsk_ring_prod*>(fill_ring_);
    auto* xsk  = static_cast<struct xsk_socket*>(xsk_socket_);
    auto* umem_base = static_cast<uint8_t*>(umem_buf_);

    uint32_t idx_rx = 0;
    const uint32_t rcvd = ::xsk_ring_cons__peek(rx, kBatchSize, &idx_rx);
    if (rcvd == 0) {
        // Wake up if needed
        if (::xsk_ring_prod__needs_wakeup(fill)) {
            ::recvfrom(::xsk_socket__fd(xsk), nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
        }
        return 0;
    }

    for (uint32_t i = 0; i < rcvd; ++i) {
        const struct xdp_desc* desc = ::xsk_ring_cons__rx_desc(rx, idx_rx++);
        const uint8_t* pkt = umem_base + desc->addr;
        const size_t   len = desc->len;
        cb(pkt, len, 0);  // hw_ts not available in basic XDP path
    }

    ::xsk_ring_cons__release(rx, rcvd);

    // Refill fill ring
    uint32_t idx_fq = 0;
    if (::xsk_ring_prod__reserve(fill, rcvd, &idx_fq) == rcvd) {
        for (uint32_t i = 0; i < rcvd; ++i) {
            *::xsk_ring_prod__fill_addr(fill, idx_fq++) =
                static_cast<uint64_t>(i) * kFrameSize;  // simplified — reuse frames
        }
        ::xsk_ring_prod__submit(fill, rcvd);
    }

    return rcvd;
}
#else
size_t XdpSocket::recv_batch_xdp(const PacketCallback& /*cb*/) noexcept {
    return 0;
}
#endif

size_t XdpSocket::recv_batch_fallback(const PacketCallback& cb) noexcept {
    auto* hdrs = static_cast<struct mmsghdr*>(msg_hdrs_);

    const int n = ::recvmmsg(fd_, hdrs, static_cast<unsigned>(kBatchSize),
                              MSG_DONTWAIT, nullptr);
    if (n <= 0) return 0;

    // Use a software clock for timestamp when HW not available
    struct timespec sw_ts{};
    ::clock_gettime(CLOCK_REALTIME, &sw_ts);
    const uint64_t sw_ns = static_cast<uint64_t>(sw_ts.tv_sec) * 1'000'000'000ULL
                         + static_cast<uint64_t>(sw_ts.tv_nsec);

    for (int i = 0; i < n; ++i) {
        const uint8_t* data = recv_bufs_[i].data;
        const size_t   len  = hdrs[i].msg_len;
        cb(data, len, sw_ns);
        // Reset for next recvmmsg call
        hdrs[i].msg_len = 0;
    }
    return static_cast<size_t>(n);
}

uint64_t XdpSocket::hw_timestamp_ns(const void* /*cmsg_data*/) noexcept {
    // Extract hardware timestamp from SO_TIMESTAMPING cmsg
    // struct scm_timestamping { struct timespec ts[3]; }
    // ts[0] = software, ts[1] = deprecated, ts[2] = hardware
    // For simplicity we return 0 here; a full implementation would parse cmsg
    return 0;
}

// ---------------------------------------------------------------------------
// close()
// ---------------------------------------------------------------------------

void XdpSocket::close() noexcept {
#ifdef HAVE_LIBBPF
    if (xsk_socket_) {
        ::xsk_socket__delete(static_cast<struct xsk_socket*>(xsk_socket_));
        xsk_socket_ = nullptr;
    }
    if (xsk_umem_) {
        auto* fill = static_cast<struct xsk_ring_prod*>(fill_ring_);
        auto* comp = static_cast<struct xsk_ring_cons*>(nullptr);
        ::xsk_umem__delete(static_cast<struct xsk_umem*>(xsk_umem_));
        delete fill;
        (void)comp;
        xsk_umem_  = nullptr;
        fill_ring_ = nullptr;
    }
    if (rx_ring_) {
        delete static_cast<struct xsk_ring_cons*>(rx_ring_);
        rx_ring_ = nullptr;
    }
#endif
    if (umem_buf_) {
        ::free(umem_buf_);
        umem_buf_ = nullptr;
    }
    if (fd_ >= 0 && !use_xdp_) {
        ::close(fd_);
        fd_ = -1;
    }
    if (recv_bufs_) { delete[] recv_bufs_; recv_bufs_ = nullptr; }
    if (msg_hdrs_)  { ::free(msg_hdrs_);  msg_hdrs_  = nullptr; }
    if (msg_iovecs_){ ::free(msg_iovecs_); msg_iovecs_= nullptr; }
}

} // namespace net
