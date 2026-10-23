// Equal-contract dynamic rank/range comparison on natural string keys.
//
// Every method implements the same operations with the same semantics:
//   contains(k), insert(k) / erase(k) returning success, rank(k) for a live key
//   (strict lower rank; callers never pass a non-live key), boundary_rank(k)
//   for an arbitrary key, and inclusive count_range(lo, hi) (0 when lo > hi).
// Every stream is generated from an independent oracle (sorted universe of
// base and arrival keys plus a Fenwick tree over live flags) before any index
// is built, so index-owned heap bytes are measured without harness noise.
// Every timed answer is checked after its stream finishes.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>

#include "counted_radix_ost.hpp"
#include "packed_rank_transport.hpp"
#ifdef HRTLI_WITH_COUNTED_ART
#include "counted_art.hpp"
#endif
#ifdef HRTLI_WITH_FENCED
#include "fenced_rank_transport.hpp"
#endif

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

using Clock = std::chrono::steady_clock;
double seconds_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

size_t heap_in_use() {
    struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
}

// NUL-delimited natural keys (the arrival file of the verified sample).
std::vector<std::string> read_nul_keys(const std::string& path, size_t expected) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "cannot open " + path);
    std::vector<std::string> keys;
    keys.reserve(expected);
    std::string key;
    while (std::getline(in, key, '\0')) keys.push_back(key);
    require(keys.size() == expected, path + ": unexpected key count");
    for (size_t i = 1; i < keys.size(); ++i) require(keys[i - 1] < keys[i], path + ": not sorted/unique");
    return keys;
}

struct Fenwick {
    std::vector<int64_t> tree;
    explicit Fenwick(size_t n) : tree(n + 1, 0) {}
    void add(size_t i, int64_t d) { for (++i; i < tree.size(); i += i & (~i + 1)) tree[i] += d; }
    int64_t prefix(size_t n) const {  // sum of [0, n)
        int64_t s = 0;
        for (; n; n -= n & (~n + 1)) s += tree[n];
        return s;
    }
    size_t find_by_order(int64_t k) const {  // smallest i with prefix(i+1) > k
        size_t pos = 0, step = 1;
        while (step * 2 < tree.size()) step *= 2;
        for (; step; step /= 2) {
            if (pos + step < tree.size() && tree[pos + step] <= k) {
                pos += step;
                k -= tree[pos];
            }
        }
        return pos;
    }
};

enum OpType : uint8_t { Contains, Insert, Erase, Rank, Boundary, Range };

struct Op {
    OpType type;
    std::string a, b;
    int64_t expected;
};

struct Stream {
    std::string name;
    std::vector<Op> ops;
    bool read_only;
    bool maintain_after = false;  // invoke the method's maintenance step after this stream
};

// Oracle over the sorted universe U = base ∪ arrivals (disjoint, both sorted).
struct Oracle {
    std::vector<std::string_view> universe;
    std::vector<uint8_t> live;
    Fenwick fen;
    int64_t live_count = 0;
    std::vector<uint32_t> live_ids, dead_ids;          // for O(1) uniform sampling
    std::vector<uint32_t> slot;                        // position of id in live_ids/dead_ids

    Oracle(const hrtli::MappedKeyStore& base, const std::vector<std::string>& arrivals)
        : fen(base.size() + arrivals.size()) {
        universe.reserve(base.size() + arrivals.size());
        size_t i = 0, j = 0;
        while (i < base.size() || j < arrivals.size()) {
            if (j == arrivals.size() || (i < base.size() && base.view(i) < arrivals[j])) {
                universe.push_back(base.view(i++));
                live.push_back(1);
            } else {
                require(i == base.size() || base.view(i) != arrivals[j], "base/arrival overlap");
                universe.push_back(arrivals[j++]);
                live.push_back(0);
            }
        }
        slot.resize(universe.size());
        for (uint32_t id = 0; id < universe.size(); ++id) {
            auto& pool = live[id] ? live_ids : dead_ids;
            slot[id] = static_cast<uint32_t>(pool.size());
            pool.push_back(id);
            if (live[id]) fen.add(id, 1);
        }
        live_count = static_cast<int64_t>(live_ids.size());
    }
    void move(uint32_t id, bool to_live) {
        auto& from = to_live ? dead_ids : live_ids;
        auto& to = to_live ? live_ids : dead_ids;
        const uint32_t s = slot[id];
        from[s] = from.back();
        slot[from[s]] = s;
        from.pop_back();
        slot[id] = static_cast<uint32_t>(to.size());
        to.push_back(id);
        live[id] = to_live;
        fen.add(id, to_live ? 1 : -1);
        live_count += to_live ? 1 : -1;
    }
    std::string key(uint32_t id) const { return std::string(universe[id]); }
    int64_t rank_of_id(uint32_t id) const { return fen.prefix(id); }
    int64_t count_ids(uint32_t lo, uint32_t hi) const { return fen.prefix(hi + 1) - fen.prefix(lo); }
};

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(uint64_t seed) : engine(seed) {}
    uint64_t below(uint64_t n) { return engine() % n; }
};

std::vector<Stream> make_streams(Oracle& oracle, size_t delta, size_t queries, uint64_t seed,
                                 size_t base_count) {
    std::vector<Stream> streams;
    Rng rng(seed);
    // 1. Writes establishing the target net delta: delta/2 base deletes and
    //    delta/2 arrival inserts, interleaved, each a successful mutation.
    {
        Stream s{"writes_establish_delta", {}, false};
        std::vector<uint32_t> deletes, inserts;
        std::vector<uint8_t> taken(oracle.universe.size(), 0);
        while (deletes.size() < delta / 2) {
            uint32_t id = oracle.live_ids[rng.below(oracle.live_ids.size())];
            if (!taken[id]) { taken[id] = 1; deletes.push_back(id); }
        }
        while (inserts.size() < delta - delta / 2) {
            uint32_t id = oracle.dead_ids[rng.below(oracle.dead_ids.size())];
            if (!taken[id]) { taken[id] = 1; inserts.push_back(id); }
        }
        std::vector<std::pair<bool, uint32_t>> order;
        for (auto id : deletes) order.push_back({false, id});
        for (auto id : inserts) order.push_back({true, id});
        for (size_t i = order.size(); i > 1; --i) std::swap(order[i - 1], order[rng.below(i)]);
        for (auto [ins, id] : order) {
            s.ops.push_back({ins ? Insert : Erase, oracle.key(id), {}, 1});
            oracle.move(id, ins);
        }
        streams.push_back(std::move(s));
    }
    auto live_key = [&]() { return oracle.live_ids[rng.below(oracle.live_ids.size())]; };
    auto dead_key = [&]() { return oracle.dead_ids[rng.below(oracle.dead_ids.size())]; };
    {
        Stream s{"member_hit", {}, true};
        for (size_t i = 0; i < queries; ++i) s.ops.push_back({Contains, oracle.key(live_key()), {}, 1});
        streams.push_back(std::move(s));
    }
    {
        Stream s{"member_miss", {}, true};
        for (size_t i = 0; i < queries; ++i) s.ops.push_back({Contains, oracle.key(dead_key()), {}, 0});
        streams.push_back(std::move(s));
    }
    {
        Stream s{"rank_live", {}, true};
        for (size_t i = 0; i < queries; ++i) {
            uint32_t id = live_key();
            s.ops.push_back({Rank, oracle.key(id), {}, oracle.rank_of_id(id)});
        }
        streams.push_back(std::move(s));
    }
    {
        Stream s{"rank_absent_boundary", {}, true};
        for (size_t i = 0; i < queries; ++i) {
            uint32_t id = dead_key();
            s.ops.push_back({Boundary, oracle.key(id), {}, oracle.rank_of_id(id)});
        }
        streams.push_back(std::move(s));
    }
    for (int64_t width : {10, 1000, 100000}) {
        Stream s{"range_live_" + std::to_string(width), {}, true};
        for (size_t i = 0; i < queries; ++i) {
            int64_t r = static_cast<int64_t>(rng.below(oracle.live_count - width + 1));
            uint32_t lo = static_cast<uint32_t>(oracle.fen.find_by_order(r));
            uint32_t hi = static_cast<uint32_t>(oracle.fen.find_by_order(r + width - 1));
            s.ops.push_back({Range, oracle.key(lo), oracle.key(hi), width});
        }
        streams.push_back(std::move(s));
    }
    {
        // Absent endpoints: lo is a non-live universe key, hi lies ~1000 universe positions later.
        Stream s{"range_absent_1000", {}, true};
        for (size_t i = 0; i < queries; ++i) {
            uint32_t lo = dead_key();
            uint32_t hi = static_cast<uint32_t>(std::min<size_t>(lo + 1000, oracle.universe.size() - 1));
            s.ops.push_back({Range, oracle.key(lo), oracle.key(hi), oracle.count_ids(lo, hi)});
        }
        streams.push_back(std::move(s));
    }
    {
        // 50% successful writes (alternating sign) and 50% ordered reads.
        Stream s{"mixed_50w_rank_range", {}, false};
        bool del = true;
        for (size_t i = 0; i < queries; ++i) {
            switch (i % 4) {
                case 0: {
                    uint32_t id = live_key();
                    s.ops.push_back({Rank, oracle.key(id), {}, oracle.rank_of_id(id)});
                    break;
                }
                case 2: {
                    int64_t r = static_cast<int64_t>(rng.below(oracle.live_count - 100 + 1));
                    uint32_t lo = static_cast<uint32_t>(oracle.fen.find_by_order(r));
                    uint32_t hi = static_cast<uint32_t>(oracle.fen.find_by_order(r + 99));
                    s.ops.push_back({Range, oracle.key(lo), oracle.key(hi), 100});
                    break;
                }
                default: {
                    uint32_t id = del ? live_key() : dead_key();
                    s.ops.push_back({del ? Erase : Insert, oracle.key(id), {}, 1});
                    oracle.move(id, !del);
                    del = !del;
                }
            }
        }
        streams.push_back(std::move(s));
    }
    (void)base_count;
    return streams;
}

// Repeated epochs: W successful writes (alternating sign), Q ordered reads
// (half live rank, half width-100 live ranges), then maintenance.
std::vector<Stream> make_lifecycle(Oracle& oracle, size_t epochs, size_t writes, size_t reads, uint64_t seed) {
    std::vector<Stream> streams;
    Rng rng(seed);
    bool del = true;
    for (size_t e = 0; e < epochs; ++e) {
        Stream w{"epoch" + std::to_string(e) + "_writes", {}, false};
        for (size_t i = 0; i < writes; ++i) {
            const uint32_t id = del ? oracle.live_ids[rng.below(oracle.live_ids.size())]
                                    : oracle.dead_ids[rng.below(oracle.dead_ids.size())];
            w.ops.push_back({del ? Erase : Insert, oracle.key(id), {}, 1});
            oracle.move(id, !del);
            del = !del;
        }
        streams.push_back(std::move(w));
        Stream r{"epoch" + std::to_string(e) + "_reads", {}, true, true};
        for (size_t i = 0; i < reads; ++i) {
            if (i % 2 == 0) {
                const uint32_t id = oracle.live_ids[rng.below(oracle.live_ids.size())];
                r.ops.push_back({Rank, oracle.key(id), {}, oracle.rank_of_id(id)});
            } else {
                const int64_t q = static_cast<int64_t>(rng.below(oracle.live_count - 99));
                r.ops.push_back({Range, oracle.key(static_cast<uint32_t>(oracle.fen.find_by_order(q))),
                                 oracle.key(static_cast<uint32_t>(oracle.fen.find_by_order(q + 99))), 100});
            }
        }
        streams.push_back(std::move(r));
    }
    return streams;
}

// ----------------------------------------------------------------- adapters
struct HrtliAdapter {
    hrtli::PackedRankTransportIndex index;
    HrtliAdapter(const hrtli::MappedKeyStore& base, const std::string& path, int eps)
        : index(path, base.size(), eps, false) {}
    bool contains(const std::string& k) const { return index.point_lookup(k); }
    bool insert(const std::string& k) { return index.insert(k); }
    bool erase(const std::string& k) { return index.remove(k); }
    int64_t rank(const std::string& k) const { return index.exact_rank(k); }
    int64_t boundary_rank(const std::string& k) const { return index.boundary_rank(k); }
    int64_t count_range(const std::string& lo, const std::string& hi) const { return index.count_range(lo, hi); }
    size_t mapped_bytes() const { return index.base_store().mapped_bytes(); }
    std::string extra() const {
        std::ostringstream o;
        o << "\"model_segments\":" << index.model_segments()
          << ",\"repaired_segments\":" << index.repaired_model_segments()
          << ",\"ledger_entries\":" << index.mutation_count()
          << ",\"ledger_inserted\":" << index.ledger().inserted_size()
          << ",\"ledger_deleted\":" << index.ledger().deleted_size()
          << ",\"ledger_bytes_estimate\":" << index.ledger().memory_bytes();
        return o.str();
    }
};

struct HrtliBinaryAdapter : HrtliAdapter {
    using HrtliAdapter::HrtliAdapter;
    bool contains(const std::string& k) const { return index.binary_point_lookup(k); }
    int64_t rank(const std::string& k) const { return index.binary_exact_rank(k); }
    int64_t boundary_rank(const std::string& k) const {
        return static_cast<int64_t>(index.base_store().lower_bound(k)) + index.ledger().prefix_delta(k);
    }
    int64_t count_range(const std::string& lo, const std::string& hi) const { return index.binary_count_range(lo, hi); }
};

struct RadixOstAdapter {
    hrtli::CountedRadixOST index;
    RadixOstAdapter(const hrtli::MappedKeyStore& base, const std::string&, int) {
        for (size_t i = 0; i < base.size(); ++i) require(index.insert(std::string(base.view(i))), "radix build");
    }
    bool contains(const std::string& k) const { return index.contains(k); }
    bool insert(const std::string& k) { return index.insert(k); }
    bool erase(const std::string& k) { return index.remove(k); }
    int64_t rank(const std::string& k) const { return index.rank(k); }
    int64_t boundary_rank(const std::string& k) const { return index.boundary_rank(k); }
    int64_t count_range(const std::string& lo, const std::string& hi) const { return index.count_range(lo, hi); }
    size_t mapped_bytes() const { return 0; }
    std::string extra() const { return "\"self_estimated_bytes\":" + std::to_string(index.memory_bytes()); }
};

struct PbdsAdapter {
    using Tree = __gnu_pbds::tree<std::string, __gnu_pbds::null_type, std::less<std::string>,
                                  __gnu_pbds::rb_tree_tag, __gnu_pbds::tree_order_statistics_node_update>;
    Tree index;
    PbdsAdapter(const hrtli::MappedKeyStore& base, const std::string&, int) {
        for (size_t i = 0; i < base.size(); ++i) require(index.insert(std::string(base.view(i))).second, "pbds build");
    }
    bool contains(const std::string& k) const { return index.find(k) != index.end(); }
    bool insert(const std::string& k) { return index.insert(k).second; }
    bool erase(const std::string& k) { return index.erase(k); }
    int64_t rank(const std::string& k) const {
        require(contains(k), "pbds rank of a non-live key");
        return static_cast<int64_t>(index.order_of_key(k));
    }
    int64_t boundary_rank(const std::string& k) const { return static_cast<int64_t>(index.order_of_key(k)); }
    int64_t count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        return static_cast<int64_t>(index.order_of_key(hi)) - static_cast<int64_t>(index.order_of_key(lo)) +
               (contains(hi) ? 1 : 0);
    }
    size_t mapped_bytes() const { return 0; }
    std::string extra() const { return "\"self_estimated_bytes\":null"; }
};

#ifdef HRTLI_WITH_COUNTED_ART
struct CountedArtAdapter {
    hrtli::CountedArt index;
    CountedArtAdapter(const hrtli::MappedKeyStore& base, const std::string&, int) {
        for (size_t i = 0; i < base.size(); ++i) require(index.insert(base.view(i)), "counted ART build");
    }
    bool contains(const std::string& k) const { return index.contains(k); }
    bool insert(const std::string& k) { return index.insert(k); }
    bool erase(const std::string& k) { return index.erase(k); }
    int64_t rank(const std::string& k) const { return index.rank(k); }
    int64_t boundary_rank(const std::string& k) const { return index.boundary_rank(k); }
    int64_t count_range(const std::string& lo, const std::string& hi) const { return index.count_range(lo, hi); }
    size_t mapped_bytes() const { return 0; }
    std::string extra() const { return "\"art_inner_nodes\":" + std::to_string(index.inner_nodes()); }
};
#endif

#ifdef HRTLI_WITH_FENCED
template <bool FullRefit, bool FencePrefix = false, bool KeySlices = false>
struct FencedAdapter {
    hrtli::FencedRankTransportIndex index;
    FencedAdapter(const hrtli::MappedKeyStore& base, const std::string& path, int eps)
        : index(path, base.size(), eps, hrtli::FencedOptions{FencePrefix, KeySlices}) {}
    bool contains(const std::string& k) const { return index.contains(k); }
    bool insert(const std::string& k) { return index.insert(k); }
    bool erase(const std::string& k) { return index.erase(k); }
    int64_t rank(const std::string& k) const { return index.rank(k); }
    int64_t boundary_rank(const std::string& k) const { return index.boundary_rank(k); }
    int64_t count_range(const std::string& lo, const std::string& hi) const { return index.count_range(lo, hi); }
    size_t mapped_bytes() const { return index.base().mapped() ? index.base().key_bytes() : 0; }
    std::string maintain() {
        const auto st = index.consolidate(FullRefit);
        // Out-of-band audit of the certificate over every key of the new generation.
        const auto a0 = Clock::now();
        const int64_t err = index.certified_max_error();
        const double audit = seconds_between(a0, Clock::now());
        require(err <= index.model().epsilon(), "consolidated generation violates its certificate");
        std::ostringstream o;
        o << std::setprecision(10) << "{\"full_refit\":" << (st.full_refit ? "true" : "false")
          << ",\"old_keys\":" << st.old_keys << ",\"new_keys\":" << st.new_keys
          << ",\"old_segments\":" << st.old_segments << ",\"new_segments\":" << st.new_segments
          << ",\"inherited_segments\":" << st.inherited_segments << ",\"dirty_segments\":" << st.dirty_segments
          << ",\"refit_keys\":" << st.refit_keys << ",\"ledger_entries\":" << st.ledger_entries
          << ",\"merge_copy_seconds\":" << st.merge_copy_seconds << ",\"refit_seconds\":" << st.refit_seconds
          << ",\"accel_seconds\":" << st.accel_seconds
          << ",\"total_seconds\":" << st.total_seconds << ",\"audited_max_error\":" << err
          << ",\"audit_seconds\":" << audit << "}";
        return o.str();
    }
    std::string extra() const {
        std::ostringstream o;
        o << "\"model_segments\":" << index.model().size() << ",\"repaired_segments\":" << index.model().repaired_segments()
          << ",\"ledger_entries\":" << index.ledger().entries()
          << ",\"promoted_buckets\":" << index.ledger().promoted_buckets()
          << ",\"ledger_bytes_estimate\":" << index.ledger().memory_bytes()
          << ",\"accelerator_bytes\":" << index.accelerator_bytes()
          << ",\"offsets_model_ledger_accel_bytes\":" << index.memory_bytes_excluding_keys();
        return o.str();
    }
};
#endif

// Methods without a maintenance step (fully dynamic competitors) report null.
template <class Index>
auto maintain_of(Index& index, int) -> decltype(index.maintain()) { return index.maintain(); }
template <class Index>
std::string maintain_of(Index&, long) { return "null"; }

template <class Index>
int64_t apply(Index& index, const Op& op) {
    switch (op.type) {
        case Contains: return index.contains(op.a);
        case Insert: return index.insert(op.a);
        case Erase: return index.erase(op.a);
        case Rank: return index.rank(op.a);
        case Boundary: return index.boundary_rank(op.a);
        case Range: return index.count_range(op.a, op.b);
    }
    return -1;
}

template <class Index>
void run(const std::string& mode, const hrtli::MappedKeyStore& base, const std::string& base_path, int eps,
         std::vector<Stream>& streams, const std::string& header) {
    size_t longest = 0;
    for (const auto& stream : streams) longest = std::max(longest, stream.ops.size());
    std::vector<int64_t> answers(longest, 0);  // harness buffer, allocated before the heap baseline
    malloc_trim(0);
    const size_t heap0 = heap_in_use();
    const auto b0 = Clock::now();
    Index index(base, base_path, eps);
    const auto b1 = Clock::now();
    const size_t heap_built = heap_in_use() - heap0;
    std::cout << std::setprecision(10) << "{" << header << ",\"mode\":\"" << mode << "\""
              << ",\"build_seconds\":" << seconds_between(b0, b1)
              << ",\"heap_bytes_after_build\":" << heap_built
              << ",\"mapped_key_bytes\":" << index.mapped_bytes() << ",\"streams\":[";
    bool first = true;
    for (auto& stream : streams) {
        if (stream.read_only) {  // untimed warm-up over a prefix of the same stream
            for (size_t i = 0; i < std::min<size_t>(stream.ops.size(), 20000); ++i) answers[i] = apply(index, stream.ops[i]);
        }
        struct rusage r0 {}, r1 {};
        getrusage(RUSAGE_SELF, &r0);
        const auto t0 = Clock::now();
        for (size_t i = 0; i < stream.ops.size(); ++i) answers[i] = apply(index, stream.ops[i]);
        const auto t1 = Clock::now();
        getrusage(RUSAGE_SELF, &r1);
        for (size_t i = 0; i < stream.ops.size(); ++i) {
            if (answers[i] != stream.ops[i].expected) {
                std::ostringstream o;
                o << mode << " " << stream.name << " op " << i << " answer " << answers[i] << " expected "
                  << stream.ops[i].expected << " key " << stream.ops[i].a;
                throw std::runtime_error(o.str());
            }
        }
        const double secs = seconds_between(t0, t1);
        std::cout << (first ? "" : ",") << "{\"name\":\"" << stream.name << "\",\"ops\":" << stream.ops.size()
                  << ",\"seconds\":" << secs << ",\"ns_per_op\":" << (secs * 1e9 / std::max<size_t>(1, stream.ops.size()))
                  << ",\"major_faults\":" << (r1.ru_majflt - r0.ru_majflt)
                  << ",\"minor_faults\":" << (r1.ru_minflt - r0.ru_minflt)
                  << ",\"heap_bytes_after\":" << (heap_in_use() - heap0) << ",\"answers_checked\":true";
        if (stream.maintain_after) {
            const auto m0 = Clock::now();
            const std::string m = maintain_of(index, 0);
            std::cout << ",\"maintenance\":" << m << ",\"maintenance_wall_seconds\":" << seconds_between(m0, Clock::now())
                      << ",\"heap_bytes_after_maintenance\":" << (heap_in_use() - heap0);
        }
        std::cout << "}";
        first = false;
    }
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    std::cout << "]," << index.extra() << ",\"max_rss_kib\":" << usage.ru_maxrss << "}" << std::endl;
}

// Out-of-path component timing for HRT-LI on the rank stream (after the writes).
void profile(const hrtli::MappedKeyStore& base, const std::string& base_path, int eps,
             std::vector<Stream>& streams, const std::string& header) {
    hrtli::PackedRankTransportIndex index(base_path, base.size(), eps, false);
    for (const auto& op : streams[0].ops)
        require(op.type == Insert ? index.insert(op.a) : index.remove(op.a), "establishing write failed");
    const auto& model = index.model();
    const auto& ledger = index.ledger();
    const auto& store = index.base_store();
    std::cout << std::setprecision(10) << "{" << header << ",\"mode\":\"profile\",\"components\":{";
    bool first = true;
    for (const char* name : {"rank_live", "rank_absent_boundary", "member_hit"}) {
        const Stream* s = nullptr;
        for (const auto& st : streams) if (st.name == name) s = &st;
        require(s != nullptr, "missing stream");
        auto time_it = [&](const char* label, auto fn) {
            uint64_t sink = 0;
            for (size_t i = 0; i < std::min<size_t>(20000, s->ops.size()); ++i) sink += fn(s->ops[i].a);
            const auto t0 = Clock::now();
            for (const auto& op : s->ops) sink += fn(op.a);
            const auto t1 = Clock::now();
            std::cout << (first ? "" : ",") << "\"" << name << "." << label << "_ns\":"
                      << seconds_between(t0, t1) * 1e9 / s->ops.size() << ",\"" << name << "." << label
                      << "_sink\":" << sink;
            first = false;
        };
        time_it("ledger_weight_walk", [&](const std::string& k) { return uint64_t(ledger.contains_inserted(k)); });
        time_it("ledger_prefix_delta", [&](const std::string& k) { return uint64_t(int64_t(ledger.prefix_delta(k))); });
        time_it("model_segment_route", [&](const std::string& k) { return uint64_t(model.segment_of(k, store)); });
        time_it("model_lower_bound", [&](const std::string& k) { return uint64_t(model.lower_bound(k, store)); });
        time_it("base_binary_lower_bound", [&](const std::string& k) { return uint64_t(store.lower_bound(k)); });
        time_it("boundary_rank_total", [&](const std::string& k) { return uint64_t(int64_t(index.boundary_rank(k))); });
        if (std::string(name) == "rank_live")
            time_it("exact_rank_total", [&](const std::string& k) { return uint64_t(int64_t(index.exact_rank(k))); });
        if (std::string(name) == "member_hit")
            time_it("point_lookup_total", [&](const std::string& k) { return uint64_t(index.point_lookup(k)); });
    }
    std::cout << "},\"model_segments\":" << index.model_segments() << ",\"ledger_entries\":" << index.mutation_count()
              << "}" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 10 || argc == 11,
                "usage: equal_contract MODE BASE_TXT ARRIVALS_NUL BASE_N ARRIVAL_N DELTA QUERIES SEED EPSILON "
                "[standard|lifecycle:EPOCHS:WRITES:READS]");
        const std::string workload = argc == 11 ? argv[10] : "standard";
        const std::string mode = argv[1], base_path = argv[2], arrivals_path = argv[3];
        const size_t base_n = std::stoull(argv[4]), arrival_n = std::stoull(argv[5]);
        const size_t delta = std::stoull(argv[6]), queries = std::stoull(argv[7]);
        const uint64_t seed = std::stoull(argv[8]);
        const int eps = std::stoi(argv[9]);
        require(delta / 2 <= base_n && delta - delta / 2 <= arrival_n, "delta exceeds available keys");
        hrtli::MappedKeyStore base(base_path, base_n);  // harness copy; not index-owned
        std::vector<std::string> arrivals = read_nul_keys(arrivals_path, arrival_n);
        Oracle oracle(base, arrivals);
        std::vector<Stream> streams;
        if (workload == "standard") {
            streams = make_streams(oracle, delta, queries, seed, base_n);
        } else {
            size_t e = 0, w = 0, r = 0;
            require(std::sscanf(workload.c_str(), "lifecycle:%zu:%zu:%zu", &e, &w, &r) == 3, "bad workload " + workload);
            streams = make_lifecycle(oracle, e, w, r, seed);
        }
        size_t max_len = 0, total_len = 0;
        for (size_t i = 0; i < base.size(); ++i) { max_len = std::max(max_len, base.view(i).size()); total_len += base.view(i).size(); }
        std::ostringstream header;
        header << "\"base_keys\":" << base_n << ",\"arrival_keys\":" << arrival_n << ",\"net_delta\":" << delta
               << ",\"queries_per_stream\":" << queries << ",\"seed\":" << seed << ",\"epsilon\":" << eps
               << ",\"workload\":\"" << workload << "\""
               << ",\"base_mean_key_bytes\":" << double(total_len) / base_n << ",\"base_max_key_bytes\":" << max_len;
        if (mode == "hrtli") run<HrtliAdapter>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "hrtli_binary") run<HrtliBinaryAdapter>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "radix_ost") run<RadixOstAdapter>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "pbds_rbtree") run<PbdsAdapter>(mode, base, base_path, eps, streams, header.str());
#ifdef HRTLI_WITH_COUNTED_ART
        else if (mode == "counted_art") run<CountedArtAdapter>(mode, base, base_path, eps, streams, header.str());
#endif
#ifdef HRTLI_WITH_FENCED
        else if (mode == "hrtli_fenced") run<FencedAdapter<false>>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "hrtli_fenced_fullrefit") run<FencedAdapter<true>>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "hrtli_fenced_r") run<FencedAdapter<false, true>>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "hrtli_fenced_rw") run<FencedAdapter<false, true, true>>(mode, base, base_path, eps, streams, header.str());
        else if (mode == "hrtli_fenced_rw_fullrefit") run<FencedAdapter<true, true, true>>(mode, base, base_path, eps, streams, header.str());
#endif
        else if (mode == "profile") profile(base, base_path, eps, streams, header.str());
        else throw std::runtime_error("unknown mode " + mode);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
