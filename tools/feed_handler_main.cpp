/// feed_handler_main.cpp — Standalone ITCH 5.0 feed handler binary.
///
/// Usage:
///   feed_handler [options]
///   Options:
///     --iface  <name>    Network interface to listen on   (default: eth0)
///     --port   <port>    UDP port                          (default: 21002)
///     --replay <path>    Replay an ITCH binary file instead of live feed
///     --stats  <secs>    Print stats every N seconds       (default: 5)
///     --help             Show this message

#include "net/xdp_socket.hpp"
#include "net/pcap_replay.hpp"
#include "itch/parser.hpp"
#include "itch/order_book.hpp"
#include "itch/spsc_ring.hpp"
#include "itch/book_event.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <memory>
#include <thread>
#include <chrono>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static void handle_signal(int) noexcept {
    g_running.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct Config {
    const char* iface        = "eth0";
    uint16_t    port         = 21002;
    const char* replay_path  = nullptr;
    int         stats_secs   = 5;
};

static void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "ITCH 5.0 Feed Handler\n"
        "Usage: %s [--iface <iface>] [--port <port>] [--replay <path>]\n"
        "          [--stats <secs>] [--help]\n\n"
        "  --iface   Network interface (default: eth0)\n"
        "  --port    UDP port          (default: 21002)\n"
        "  --replay  Replay ITCH binary file\n"
        "  --stats   Stats interval in seconds (default: 5)\n",
        argv0);
}

static Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iface") == 0 && i + 1 < argc) {
            cfg.iface = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            cfg.replay_path = argv[++i];
        } else if (std::strcmp(argv[i], "--stats") == 0 && i + 1 < argc) {
            cfg.stats_secs = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            std::exit(0);
        }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Strategy thread: reads BookEvents from the SPSC ring
// ---------------------------------------------------------------------------

using EventRing = itch::SpscRing<itch::BookEvent, 65536>;

static void strategy_thread(EventRing& ring) noexcept {
    uint64_t total = 0;
    while (g_running.load(std::memory_order_relaxed) || !ring.empty()) {
        auto ev = ring.pop();
        if (!ev) {
            // Spin briefly, then yield
            for (int i = 0; i < 32; ++i) {
                if (ring.pop()) { ++total; break; }
            }
            std::this_thread::yield();
            continue;
        }
        ++total;
        // Strategy hook point: act on ev->best_bid / best_ask etc.
        (void)ev;
    }
    std::fprintf(stdout, "Strategy thread processed %lu events\n", total);
}

// ---------------------------------------------------------------------------
// Stats printer
// ---------------------------------------------------------------------------

static void print_stats(const itch::Parser::Stats& s,
                         double elapsed_sec) noexcept {
    std::fprintf(stdout,
        "\n=== ITCH Feed Handler Stats (%.1fs) ===\n"
        "  Total messages : %lu\n"
        "  Add orders     : %lu\n"
        "  Cancels        : %lu\n"
        "  Deletes        : %lu\n"
        "  Replaces       : %lu\n"
        "  Executions     : %lu\n"
        "  Unknown        : %lu\n"
        "  Throughput     : %.2f Mmsg/s\n",
        elapsed_sec,
        s.total_messages,
        s.add_orders,
        s.cancels,
        s.deletes,
        s.replaces,
        s.executions,
        s.unknown_messages,
        static_cast<double>(s.total_messages) / elapsed_sec / 1e6);
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    const Config cfg = parse_args(argc, argv);

    // Print ASCII architecture banner
    std::puts(
        "\n"
        "  NIC\n"
        "   |\n"
        "   v\n"
        "  AF_XDP UMEM  (kUmemFrames * 2048 B)\n"
        "   |\n"
        "   v\n"
        "  XDP Program  (BPF: filter by port)\n"
        "   |\n"
        "   v\n"
        "  Userspace Ring  [XdpSocket::recv_batch]\n"
        "   |\n"
        "   v\n"
        "  Parser Thread  [Parser::parse_stream]\n"
        "    |    \\\n"
        "    |     --> OrderBook updates (lock-free levels + BBO atomic)\n"
        "    |\n"
        "   SPSC Ring  [SpscRing<BookEvent, 65536>]\n"
        "    |\n"
        "    v\n"
        "  Strategy Thread  [user hook]\n"
        "\n");

    // Allocate book pool
    static constexpr size_t kBookCount = 65536;
    auto books = std::make_unique<itch::OrderBook[]>(kBookCount);

    // Build parser with a simple callback that pushes BookEvents
    EventRing event_ring;

    itch::Callbacks cbs{};
    cbs.on_add_order = [&](const itch::MsgAddOrder& m) {
        const auto* book = &books[m.stock_locate];
        const auto  bbo  = book->bbo();
        itch::BookEvent ev{};
        ev.hardware_ts_ns = 0;
        ev.itch_ts_ns     = m.timestamp_ns;
        ev.price          = m.price;
        ev.order_ref      = m.order_ref_num;
        ev.best_bid       = bbo.bid_price;
        ev.best_ask       = bbo.ask_price;
        ev.qty            = m.shares;
        ev.stock_locate   = m.stock_locate;
        ev.event_type     = static_cast<uint8_t>(itch::EventType::ADD);
        ev.side           = static_cast<uint8_t>(m.buy_sell);
        ev.best_bid_qty   = static_cast<uint32_t>(bbo.bid_qty);
        ev.best_ask_qty   = static_cast<uint32_t>(bbo.ask_qty);
        (void)event_ring.push(ev);  // best-effort — drop if ring full
    };

    itch::Parser parser(books.get(), kBookCount, std::move(cbs));

    // Start strategy thread
    std::thread strat_thread(strategy_thread, std::ref(event_ring));

    const auto start_time = std::chrono::steady_clock::now();
    auto       last_stat  = start_time;

    if (cfg.replay_path) {
        // ---- Replay mode ----
        std::fprintf(stdout, "Replaying ITCH binary: %s\n", cfg.replay_path);

        net::PcapReplayer replayer;
        if (!replayer.load_itch_binary(cfg.replay_path)) {
            std::fprintf(stderr, "Failed to load %s\n", cfg.replay_path);
            g_running.store(false);
            strat_thread.join();
            return 1;
        }

        std::fprintf(stdout, "Loaded %zu messages (%zu bytes)\n",
                     replayer.message_count(), replayer.byte_count());

        replayer.replay_all([&](const uint8_t* msg, size_t len) {
            itch::MessageView mv{msg, static_cast<uint16_t>(len)};
            parser.parse(mv, 0);
        });

    } else {
        // ---- Live feed mode ----
        std::fprintf(stdout, "Listening on %s port %u\n", cfg.iface, cfg.port);

        net::XdpSocket sock;
        if (!sock.init(cfg.iface, cfg.port)) {
            std::fprintf(stderr, "Failed to open socket on %s\n", cfg.iface);
            g_running.store(false);
            strat_thread.join();
            return 1;
        }
        std::fprintf(stdout, "Using %s path\n",
                     sock.using_xdp() ? "AF_XDP" : "recvmmsg+SO_BUSY_POLL");

        while (g_running.load(std::memory_order_relaxed)) {
            sock.recv_batch([&](const uint8_t* pkt, size_t pkt_len, uint64_t hw_ts) {
                // ITCH messages arrive as length-prefixed payloads in UDP
                parser.parse_stream(pkt, pkt_len, hw_ts);
            });

            // Periodic stats
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(
                    now - last_stat).count() >= cfg.stats_secs) {
                const double elapsed = std::chrono::duration<double>(
                    now - start_time).count();
                print_stats(parser.stats(), elapsed);
                last_stat = now;
            }
        }
        sock.close();
    }

    g_running.store(false);
    strat_thread.join();

    const auto end_time = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    print_stats(parser.stats(), elapsed);

    return 0;
}
