/// bm_itch.cpp — Google Benchmark suite for the ITCH feed handler.
///
/// Benchmarks:
///   BM_ITCHParse           — raw parse throughput from a pre-built corpus
///   BM_BookUpdate_Add      — single AddOrder → BBO latency
///   BM_BookUpdate_Cancel   — single OrderCancel latency
///   BM_BookUpdate_Replace  — single OrderReplace latency
///   BM_SustainedThroughput — 10-second run with HDRHistogram p99.9 report
///
/// The corpus is built in a fixture so no I/O occurs during measurement.

#include <benchmark/benchmark.h>

#include "itch/parser.hpp"
#include "itch/order_book.hpp"
#include "itch/message_view.hpp"
#include "itch/book_event.hpp"
#include "itch/spsc_ring.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <memory>
#include <algorithm>
#include <numeric>
#include <random>

// ---------------------------------------------------------------------------
// HDRHistogram-style simple recorder (no external dep)
// ---------------------------------------------------------------------------
// For a production build, link against hdrhistogram_c and use hdr_histogram.
// Here we provide a lightweight quantile histogram sufficient for reporting.

class LatencyHistogram {
public:
    static constexpr size_t kBuckets = 1024;

    void record(uint64_t ns) noexcept {
        ++total_;
        sum_ns_ += ns;
        if (ns < min_ns_) min_ns_ = ns;
        if (ns > max_ns_) max_ns_ = ns;
        size_t bucket = bucket_for(ns);
        ++counts_[bucket];
    }

    uint64_t percentile(double pct) const noexcept {
        const uint64_t target = static_cast<uint64_t>(static_cast<double>(total_) * pct / 100.0);
        uint64_t cum = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            cum += counts_[i];
            if (cum >= target) return bucket_upper(i);
        }
        return max_ns_;
    }

    uint64_t min() const noexcept { return min_ns_; }
    uint64_t max() const noexcept { return max_ns_; }
    uint64_t mean() const noexcept { return total_ ? sum_ns_ / total_ : 0; }
    uint64_t count() const noexcept { return total_; }

    void reset() noexcept {
        total_ = 0; sum_ns_ = 0;
        min_ns_ = UINT64_MAX; max_ns_ = 0;
        counts_.fill(0);
    }

private:
    static size_t bucket_for(uint64_t ns) noexcept {
        // Logarithmic bucketing: bucket = log2(ns) * 64
        if (ns == 0) return 0;
        const int lz = __builtin_clzll(ns);
        const size_t b = static_cast<size_t>((63 - lz) * 16);
        return (b < kBuckets) ? b : kBuckets - 1;
    }
    static uint64_t bucket_upper(size_t b) noexcept {
        return 1ULL << ((b / 16) + 1);
    }

    uint64_t total_   = 0;
    uint64_t sum_ns_  = 0;
    uint64_t min_ns_  = UINT64_MAX;
    uint64_t max_ns_  = 0;
    std::array<uint64_t, kBuckets> counts_{};
};

// ---------------------------------------------------------------------------
// Synthetic ITCH corpus builder
// ---------------------------------------------------------------------------

/// Build a length-prefixed ITCH binary stream in memory.
/// Generates a realistic mix of Add, Execute, Cancel, Delete, Replace messages
/// so the book stays populated throughout the benchmark.

struct CorpusMessage {
    std::array<uint8_t, 40> data;
    uint16_t len;
};

static void write_be16(uint8_t* p, uint16_t v) noexcept {
    const uint16_t be = __builtin_bswap16(v);
    __builtin_memcpy(p, &be, 2);
}
static void write_be32(uint8_t* p, uint32_t v) noexcept {
    const uint32_t be = __builtin_bswap32(v);
    __builtin_memcpy(p, &be, 4);
}
static void write_be64(uint8_t* p, uint64_t v) noexcept {
    const uint64_t be = __builtin_bswap64(v);
    __builtin_memcpy(p, &be, 8);
}
static void write_ts48(uint8_t* p, uint64_t ns) noexcept {
    // Write 6 bytes big-endian from low 48 bits
    uint8_t buf[8];
    write_be64(buf, ns);
    __builtin_memcpy(p, buf + 2, 6);
}

class ItchCorpus {
public:
    // Use 100 symbols with 200 orders each = 20K seed orders (well within 0.5 LF limit
    // of 65536 on a per-book ORDER_MAP_CAP=1<<17 map). Total corpus ~500K messages.
    static constexpr size_t kNSymbols    = 100;
    static constexpr size_t kOrdersPerSym = 200;
    static constexpr size_t kTotalMsgs   = 500'000;

    ItchCorpus() { build(); }

    [[nodiscard]] const uint8_t* data() const noexcept { return stream_.data(); }
    [[nodiscard]] size_t         size() const noexcept { return stream_.size(); }
    [[nodiscard]] size_t    msg_count() const noexcept { return msg_count_; }

    // Pre-built individual messages for single-op benchmarks
    struct alignas(64) SingleMsg { uint8_t buf[40]; uint16_t len; };

    SingleMsg add_msg{};
    SingleMsg cancel_msg{};
    SingleMsg replace_msg{};
    SingleMsg delete_msg{};

private:
    std::vector<uint8_t> stream_;
    size_t               msg_count_ = 0;

    void build() {
        stream_.reserve(kTotalMsgs * 30);

        std::mt19937_64 rng(0xDEADBEEF42ULL);

        // Track live orders so we can cancel/delete/replace them
        struct LiveOrder { uint64_t ref; uint16_t locate; uint8_t side; };
        std::vector<LiveOrder> live;
        live.reserve(kNSymbols * kOrdersPerSym);

        uint64_t next_ref = 1;
        uint64_t ts_ns    = 34'200'000'000'000ULL;  // 9:30 AM in ns since midnight

        auto emit = [&](const uint8_t* msg, uint16_t msg_len) {
            uint8_t len_buf[2];
            write_be16(len_buf, msg_len);
            stream_.insert(stream_.end(), len_buf, len_buf + 2);
            stream_.insert(stream_.end(), msg, msg + msg_len);
            ++msg_count_;
        };

        // Seed with AddOrder messages for each symbol
        for (size_t sym = 1; sym <= kNSymbols; ++sym) {
            const uint16_t locate = static_cast<uint16_t>(sym);
            for (size_t j = 0; j < kOrdersPerSym; ++j) {
                uint8_t buf[36]{};
                buf[0] = 'A';
                write_be16(buf + 1, locate);
                write_ts48(buf + 5, ts_ns += 1000);
                write_be64(buf + 11, next_ref);
                buf[19] = (j % 2 == 0) ? 'B' : 'S';
                write_be32(buf + 20, 100 + static_cast<uint32_t>(j % 900));
                // stock: "SYM00001" etc
                buf[24] = 'S'; buf[25] = 'Y'; buf[26] = 'M';
                buf[27] = '0' + static_cast<uint8_t>(sym / 10000 % 10);
                buf[28] = '0' + static_cast<uint8_t>(sym / 1000 % 10);
                buf[29] = '0' + static_cast<uint8_t>(sym / 100 % 10);
                buf[30] = '0' + static_cast<uint8_t>(sym / 10 % 10);
                buf[31] = '0' + static_cast<uint8_t>(sym % 10);
                // price: $10 to $100 in $0.0001 units
                const uint32_t price = 100000 + static_cast<uint32_t>(rng() % 900000);
                write_be32(buf + 32, price);
                emit(buf, 36);
                live.push_back({next_ref++, locate, buf[19]});
            }
        }

        // Save one add/cancel/replace/delete for single-op benchmarks
        {
            const uint16_t locate = 1;
            // add_msg
            {
                auto& m = add_msg;
                m.len = 36;
                m.buf[0] = 'A';
                write_be16(m.buf + 1, locate);
                write_ts48(m.buf + 5, ts_ns);
                write_be64(m.buf + 11, 0xDEAD'BEEF'0001ULL);
                m.buf[19] = 'B';
                write_be32(m.buf + 20, 200);
                m.buf[24] = 'A'; m.buf[25] = 'A'; m.buf[26] = 'P'; m.buf[27] = 'L';
                m.buf[28] = ' '; m.buf[29] = ' '; m.buf[30] = ' '; m.buf[31] = ' ';
                write_be32(m.buf + 32, 1500000);  // $150.0000
            }
            // cancel_msg — cancel 100 shares from first seeded order
            {
                auto& m = cancel_msg;
                m.len = 23;
                m.buf[0] = 'X';
                write_be16(m.buf + 1, locate);
                write_ts48(m.buf + 5, ts_ns);
                write_be64(m.buf + 11, 1);  // ref 1
                write_be32(m.buf + 19, 50);
            }
            // replace_msg
            {
                auto& m = replace_msg;
                m.len = 35;
                m.buf[0] = 'U';
                write_be16(m.buf + 1, locate);
                write_ts48(m.buf + 5, ts_ns);
                write_be64(m.buf + 11, 1);              // orig_ref
                write_be64(m.buf + 19, 0xFFFF'0001ULL); // new_ref
                write_be32(m.buf + 27, 150);            // new shares
                write_be32(m.buf + 31, 1510000);        // new price
            }
            // delete_msg
            {
                auto& m = delete_msg;
                m.len = 19;
                m.buf[0] = 'D';
                write_be16(m.buf + 1, locate);
                write_ts48(m.buf + 5, ts_ns);
                write_be64(m.buf + 11, 2);  // ref 2
            }
        }

        // Mix in cancels, deletes, replaces for the sustained benchmark
        if (!live.empty()) {
            const size_t remaining = kTotalMsgs - msg_count_;
            for (size_t i = 0; i < remaining && !live.empty(); ++i) {
                const size_t idx = rng() % live.size();
                const auto&  lo  = live[idx];
                const uint8_t op = static_cast<uint8_t>(rng() % 3);

                if (op == 0) {
                    // Cancel
                    uint8_t buf[23]{};
                    buf[0] = 'X';
                    write_be16(buf + 1, lo.locate);
                    write_ts48(buf + 5, ts_ns += 1000);
                    write_be64(buf + 11, lo.ref);
                    write_be32(buf + 19, 50);
                    emit(buf, 23);
                } else if (op == 1) {
                    // Delete
                    uint8_t buf[19]{};
                    buf[0] = 'D';
                    write_be16(buf + 1, lo.locate);
                    write_ts48(buf + 5, ts_ns += 1000);
                    write_be64(buf + 11, lo.ref);
                    emit(buf, 19);
                    // O(1) erase: swap with last element then pop
                    live[idx] = live.back();
                    live.pop_back();
                } else {
                    // Replace → new ref
                    const uint64_t new_ref = next_ref++;
                    uint8_t buf[35]{};
                    buf[0] = 'U';
                    write_be16(buf + 1, lo.locate);
                    write_ts48(buf + 5, ts_ns += 1000);
                    write_be64(buf + 11, lo.ref);
                    write_be64(buf + 19, new_ref);
                    write_be32(buf + 27, 100);
                    write_be32(buf + 31, 1500000);
                    emit(buf, 35);
                    live[idx].ref = new_ref;
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Fixture: shared corpus + book pool, allocated once per process
// ---------------------------------------------------------------------------

struct Fixture {
    // Corpus uses stock_locate values 1..kNSymbols (100).
    // We allocate 256 books to cover all locate values used by the corpus
    // without the 195 GiB allocation that 65536 OrderBook objects would require.
    // A production handler would use a sparse map or allocate only seen locates.
    static constexpr size_t kBookCount = 256;

    ItchCorpus corpus;

    // Book pool: pre-allocated, one per stock-locate in [0, kBookCount)
    std::unique_ptr<itch::OrderBook[]> books;

    itch::Parser parser;

    Fixture()
        : books(std::make_unique<itch::OrderBook[]>(kBookCount))
        , parser(books.get(), kBookCount, itch::Callbacks{})
    {
        // Pre-parse the seed messages so books are populated for cancel/replace benches
        parser.parse_stream(corpus.data(), corpus.size(), 0);
    }
};

// Lazy singleton — allocated once for the process lifetime
static Fixture& get_fixture() {
    static Fixture f;
    return f;
}

// ---------------------------------------------------------------------------
// BM_ITCHParse — parse throughput
// ---------------------------------------------------------------------------

static void BM_ITCHParse(benchmark::State& state) {
    auto& fix = get_fixture();
    const uint8_t* buf  = fix.corpus.data();
    const size_t   size = fix.corpus.size();

    // Fresh books + parser for this benchmark (don't reuse seeded state)
    static constexpr size_t kBks = 256;
    auto books = std::make_unique<itch::OrderBook[]>(kBks);
    itch::Parser p(books.get(), kBks, itch::Callbacks{});

    size_t msgs = 0;
    for (auto _ : state) {
        msgs += p.parse_stream(buf, size, 0);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(msgs));
    state.SetBytesProcessed(
        static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
    state.counters["msgs/s"] = benchmark::Counter(
        static_cast<double>(msgs),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_ITCHParse)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(5.0);

// ---------------------------------------------------------------------------
// BM_BookUpdate_Add — isolated AddOrder → BBO update latency
// ---------------------------------------------------------------------------

static void BM_BookUpdate_Add(benchmark::State& state) {
    // Benchmark add_order latency using a fixed ref pool.
    // We alternate between two pools (A and B) so the map is always bounded.
    // Pool A: refs [1, kPool]. Pool B: refs [kPool+1, 2*kPool].
    // Iteration 2k:   delete from A[k%kPool], add to B[k%kPool]
    // Iteration 2k+1: delete from B[k%kPool], add to A[k%kPool]
    static constexpr uint64_t kPool = 512;

    itch::OrderBook book;
    uint64_t ts = 34'200'000'000'000ULL;

    // Seed pool A
    for (uint64_t i = 1; i <= kPool; ++i) {
        book.add_order(i, 1'500'000 - i * 100, 100, 'B', ts++);
    }

    uint64_t iter = 0;
    for (auto _ : state) {
        const uint64_t slot = iter % kPool;
        if ((iter / kPool) % 2 == 0) {
            // Delete from A, add to B
            book.delete_order(slot + 1,          ts++);
            book.add_order(kPool + slot + 1, 1'500'000 - slot * 100, 100, 'B', ts++);
        } else {
            // Delete from B, add to A
            book.delete_order(kPool + slot + 1,  ts++);
            book.add_order(slot + 1,         1'500'000 - slot * 100, 100, 'B', ts++);
        }
        benchmark::ClobberMemory();
        ++iter;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_BookUpdate_Add)
    ->Unit(benchmark::kNanosecond)
    ->Iterations(10'000'000);

// ---------------------------------------------------------------------------
// BM_BookUpdate_Cancel
// ---------------------------------------------------------------------------

static void BM_BookUpdate_Cancel(benchmark::State& state) {
    // Partially cancel orders from a fixed pool.
    // Each order starts with 1 billion shares so partial cancels of 1 share
    // never exhaust the order — no reseeding needed, map stays stable.
    static constexpr uint64_t kPool    = 512;
    static constexpr uint32_t kInitQty = 1'000'000'000u;

    itch::OrderBook book;
    uint64_t ts = 34'200'000'000'000ULL;

    for (uint64_t i = 1; i <= kPool; ++i) {
        book.add_order(i, 1'500'000 - i * 100, kInitQty, 'B', ts++);
    }

    uint64_t iter = 0;
    for (auto _ : state) {
        const uint64_t cur_ref = (iter % kPool) + 1;
        book.cancel_order(cur_ref, 1, ts++);  // partial cancel of 1 share
        ++iter;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_BookUpdate_Cancel)
    ->Unit(benchmark::kNanosecond)
    ->Iterations(10'000'000);

// ---------------------------------------------------------------------------
// BM_BookUpdate_Replace
// ---------------------------------------------------------------------------

static void BM_BookUpdate_Replace(benchmark::State& state) {
    // Alternate between two ref pools: original refs 1..512, alternates 10001..10512.
    // Each iteration: replace orig→alt (or alt→orig), keeping map size stable.
    static constexpr uint64_t kPool = 256;

    itch::OrderBook book;
    uint64_t ts = 34'200'000'000'000ULL;

    // Seed with kPool bid orders at refs [1, kPool]
    for (uint64_t i = 1; i <= kPool; ++i) {
        book.add_order(i, 1'500'000 - i * 100, 100, 'B', ts++);
    }

    uint64_t iter = 0;
    for (auto _ : state) {
        // Ping-pong: even iterations replace 1..kPool → 10001..10kPool
        //            odd iterations replace back
        const uint64_t slot = iter % kPool;
        const bool ping = ((iter / kPool) % 2) == 0;
        const uint64_t from_ref = ping ? (slot + 1)         : (10001 + slot);
        const uint64_t to_ref   = ping ? (10001 + slot)     : (slot + 1);
        book.replace_order(from_ref, to_ref, 100, 1'500'000 - slot * 100, ts++);
        benchmark::ClobberMemory();
        ++iter;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_BookUpdate_Replace)
    ->Unit(benchmark::kNanosecond)
    ->Iterations(10'000'000);

// ---------------------------------------------------------------------------
// BM_SustainedThroughput — 10-second run with latency histogram
// ---------------------------------------------------------------------------

static void BM_SustainedThroughput(benchmark::State& state) {
    auto& fix = get_fixture();

    // Fresh books for this run
    static constexpr size_t kBks = 256;
    auto books = std::make_unique<itch::OrderBook[]>(kBks);
    itch::Parser p(books.get(), kBks, itch::Callbacks{});

    const uint8_t* buf      = fix.corpus.data();
    const size_t   buf_size = fix.corpus.size();

    LatencyHistogram hist;
    size_t total_msgs = 0;

    struct timespec t0{}, t1{};

    for (auto _ : state) {
        ::clock_gettime(CLOCK_MONOTONIC, &t0);
        const size_t n = p.parse_stream(buf, buf_size, 0);
        ::clock_gettime(CLOCK_MONOTONIC, &t1);

        const uint64_t elapsed_ns =
            static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1'000'000'000ULL
            + static_cast<uint64_t>(t1.tv_nsec) - static_cast<uint64_t>(t0.tv_nsec);

        if (n > 0) {
            const uint64_t per_msg_ns = elapsed_ns / n;
            hist.record(per_msg_ns);
        }
        total_msgs += n;
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(static_cast<int64_t>(total_msgs));
    state.counters["p50_ns"]   = benchmark::Counter(static_cast<double>(hist.percentile(50.0)));
    state.counters["p99_ns"]   = benchmark::Counter(static_cast<double>(hist.percentile(99.0)));
    state.counters["p99_9_ns"] = benchmark::Counter(static_cast<double>(hist.percentile(99.9)));
    state.counters["min_ns"]   = benchmark::Counter(static_cast<double>(hist.min()));
    state.counters["max_ns"]   = benchmark::Counter(static_cast<double>(hist.max()));
    state.counters["mean_ns"]  = benchmark::Counter(static_cast<double>(hist.mean()));
    state.counters["msgs/s"]   = benchmark::Counter(
        static_cast<double>(total_msgs),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_SustainedThroughput)
    ->Unit(benchmark::kMillisecond)
    ->MinTime(10.0);

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

BENCHMARK_MAIN();
