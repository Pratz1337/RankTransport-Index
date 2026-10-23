// Same-state ablation of learned, binary-search and exact-hash query paths.
// Only supports the audited every-stride-th interleaved Common Crawl split.
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

#include "packed_rank_transport.hpp"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
static void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}
struct Probe { std::string key; size_t source_id; };
struct Sample { double ns; uint64_t checksum; };

template<class Fn>
Sample measure(size_t n, Fn fn, const std::vector<int>& expected) {
    require(n > 0 && expected.size() == n, "empty or mismatched expected stream");
    std::vector<int> output(n);
    for (size_t i = 0; i < std::min<size_t>(n, 10000); ++i)
        require(fn(i) == expected[i], "warmup oracle mismatch");
    auto started = Clock::now();
    for (size_t i = 0; i < n; ++i) output[i] = fn(i);
    double elapsed = ms(started);
    uint64_t checksum = 0;
    for (size_t i = 0; i < n; ++i) {
        require(output[i] == expected[i], "timed per-operation oracle mismatch");
        checksum += static_cast<uint64_t>(output[i]);
    }
    return {elapsed * 1e6 / n, checksum};
}

int main(int argc, char** argv) {
    try {
        if (argc != 8 && argc != 9) {
            std::cerr << "usage: benchmark_packed_query_paths BASE INSERT BASE_COUNT STRIDE EPSILON QUERIES REPEATS [all|learned_binary]\n";
            return 2;
        }
        const size_t base_count = std::stoull(argv[3]);
        const size_t stride = std::stoull(argv[4]);
        const int epsilon = std::stoi(argv[5]);
        const size_t queries = std::stoull(argv[6]);
        const size_t repeats = std::stoull(argv[7]);
        const std::string paths = argc == 9 ? argv[8] : "all";
        require(paths == "all" || paths == "learned_binary", "unknown query-path configuration");
        const size_t path_count = paths == "all" ? 3 : 2;
        require(stride > 1 && base_count % (stride - 1) == 0 && queries > 0 && repeats >= 3,
                "invalid interleaved split or repetitions");
        require(path_count != 2 || repeats % 2 == 0, "two-path rounds must balance method order");
        const size_t inserted_count = base_count / (stride - 1);
        const size_t total = base_count + inserted_count;
        std::ifstream input(argv[2], std::ios::binary);
        require(static_cast<bool>(input), "missing insertion file");
        std::vector<std::string> inserts;
        for (std::string line; std::getline(input, line);) inserts.push_back(line);
        require(inserts.size() == inserted_count && std::is_sorted(inserts.begin(), inserts.end()),
                "unexpected insert cardinality/order");
        auto started = Clock::now();
        // Each comparison uses one state; the two-path run does not allocate a hash.
        hrtli::PackedRankTransportIndex index(argv[1], base_count, epsilon, path_count == 3);
        const double construction = ms(started);
        require(index.base_model_max_error() <= epsilon, "exhaustive base certificate failed");
        const auto& base = index.base_store();
        // The expected ranks come from source positions, not index lower_bound.
        for (size_t j = 0; j < inserts.size(); ++j) {
            const size_t pos = (j + 1) * (stride - 1);
            require(base.view(pos - 1) < inserts[j], "insert left bracket violates source split");
            require(pos == base_count || inserts[j] < base.view(pos), "insert right bracket violates source split");
        }
        auto from_id = [&](size_t id) {
            if ((id + 1) % stride == 0) return Probe{inserts[id / stride], id};
            return Probe{std::string(base.view(id - id / stride)), id};
        };
        std::mt19937_64 rng(20260905);
        std::vector<size_t> order(inserted_count);
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        started = Clock::now();
        for (size_t j : order) require(index.insert(inserts[j]), "insert rejected");
        const double insert_ms = ms(started);

        std::vector<size_t> deleted_ids;
        std::vector<std::string> deletes;
        for (size_t j = 0; j < inserted_count; ++j) {
            const size_t pos = j * (stride - 1);
            deleted_ids.push_back(pos + pos / (stride - 1));
            deletes.emplace_back(base.view(pos));
        }
        started = Clock::now();
        for (size_t j : order) require(index.remove(deletes[j]), "base delete rejected");
        const double delete_ms = ms(started);
        auto live_rank = [&](size_t id) {
            return static_cast<int>(id - (std::lower_bound(deleted_ids.begin(), deleted_ids.end(), id) - deleted_ids.begin()));
        };
        std::vector<Probe> uniform, balanced, missing;
        uniform.reserve(queries); balanced.reserve(queries); missing.reserve(queries);
        std::uniform_int_distribution<size_t> original(0, total - 1);
        for (size_t i = 0; i < queries; ++i) {
            size_t id;
            do { id = original(rng); } while (std::binary_search(deleted_ids.begin(), deleted_ids.end(), id));
            uniform.push_back(from_id(id));
            if (i & 1) {
                const size_t j = rng() % inserted_count;
                balanced.push_back(from_id((j + 1) * stride - 1));
            } else {
                do { id = original(rng); } while ((id + 1) % stride == 0 ||
                        std::binary_search(deleted_ids.begin(), deleted_ids.end(), id));
                balanced.push_back(from_id(id));
            }
            missing.push_back(from_id(deleted_ids[rng() % deleted_ids.size()]));
        }
        std::vector<int> hits(queries, 1), misses(queries, 0), ranks;
        for (const auto& q : uniform) ranks.push_back(live_rank(q.source_id));
        size_t fallback_stored = 0, fallback_absent = 0;
        for (const auto& q : balanced) {
            bool fallback;
            const size_t pos = index.learned_base_lower_bound(q.key, &fallback);
            const size_t expected = q.source_id - q.source_id / stride;
            require(pos == expected, "learned base lower_bound disagrees with source position");
            if ((q.source_id + 1) % stride == 0) fallback_absent += fallback;
            else fallback_stored += fallback;
        }
        require(fallback_stored == 0, "stored key required global fallback");

        const size_t range_n = std::min<size_t>(queries, 100000);
        std::vector<Probe> left, right;
        std::vector<int> range_expected;
        for (size_t i = 0; i < range_n; ++i) {
            size_t lo = original(rng);
            size_t hi = std::min(total - 1, lo + (i % 10001));
            if (i % 13 == 0) std::swap(lo, hi); // reversed boundaries
            left.push_back(from_id(lo)); right.push_back(from_id(hi));
            const size_t erased = std::upper_bound(deleted_ids.begin(), deleted_ids.end(), hi) -
                                  std::lower_bound(deleted_ids.begin(), deleted_ids.end(), lo);
            range_expected.push_back(lo > hi ? 0 : static_cast<int>(hi - lo + 1 - erased));
        }
        std::cout << std::setprecision(12) << "{\"base_keys\":" << base_count
                  << ",\"selected_keys\":" << total << ",\"epsilon\":" << epsilon
                  << ",\"model_segments\":" << index.model_segments()
                  << ",\"repaired_segments\":" << index.repaired_model_segments()
                  << ",\"construction_ms\":" << construction << ",\"insert_ms\":" << insert_ms
                  << ",\"base_delete_ms\":" << delete_ms << ",\"queries\":" << queries
                  << ",\"range_queries\":" << range_n << ",\"seed\":20260905"
                  << ",\"query_paths\":\"" << paths << "\",\"fingerprint_capacity\":" << index.fingerprint_capacity()
                  << ",\"stored_query_fallbacks\":" << fallback_stored
                  << ",\"inserted_query_fallbacks\":" << fallback_absent
                  << ",\"trial_design\":\"paired same-state rotated query paths; not independent builds\""
                  << ",\"rows\":[";
        bool first = true;
        for (size_t trial = 0; trial < repeats; ++trial) {
            for (size_t offset = 0; offset < path_count; ++offset) {
                const size_t mode = (trial + offset) % path_count;
                auto membership = [&](const Probe& q) {
                    if (mode == 0) return index.learned_point_lookup(q.key);
                    if (mode == 1) return index.binary_point_lookup(q.key);
                    return index.point_lookup(q.key);
                };
                Sample u = measure(queries, [&](size_t i) { return membership(uniform[i]); }, hits);
                Sample b = measure(queries, [&](size_t i) { return membership(balanced[i]); }, hits);
                Sample m = measure(queries, [&](size_t i) { return membership(missing[i]); }, misses);
                Sample r = measure(queries, [&](size_t i) { return mode == 0 ?
                    index.learned_exact_rank(uniform[i].key) : index.binary_exact_rank(uniform[i].key); }, ranks);
                Sample c = measure(range_n, [&](size_t i) { return mode == 0 ?
                    index.learned_count_range(left[i].key, right[i].key) :
                    index.binary_count_range(left[i].key, right[i].key); }, range_expected);
                if (!first) std::cout << ',';
                first = false;
                std::cout << "{\"trial\":" << trial + 1 << ",\"mode\":\""
                          << std::array<const char*, 3>{"learned", "binary", "fingerprint"}[mode]
                          << "\",\"uniform_membership_ns\":" << u.ns << ",\"balanced_membership_ns\":" << b.ns
                          << ",\"deleted_membership_ns\":" << m.ns << ",\"uniform_rank_ns\":" << r.ns
                          << ",\"range_ns\":" << c.ns << ",\"rank_checksum\":" << r.checksum
                          << ",\"range_checksum\":" << c.checksum << '}';
            }
        }
        started = Clock::now();
        for (size_t j : order) require(index.insert(deletes[j]), "base reinsertion failed");
        const double base_reinsert_ms = ms(started);
        require(index.mutation_count() == inserted_count, "base annihilation did not remove tombstones");
        for (const auto& q : uniform) require(index.learned_exact_rank(q.key) == static_cast<int>(q.source_id),
                                             "rank after base reinsertion differs from original ID");
        started = Clock::now();
        for (size_t j : order) require(index.remove(inserts[j]), "inserted-key deletion failed");
        const double inserted_delete_ms = ms(started);
        require(index.mutation_count() == 0, "final mutation state not empty");
        for (const auto& key : inserts) require(!index.point_lookup(key), "deleted insertion remains visible");
        for (size_t j = 0; j < deleted_ids.size(); ++j)
            require(index.learned_exact_rank(deletes[j]) == static_cast<int>(j * (stride - 1)),
                    "original base rank not restored");
        std::cout << "],\"base_reinsert_ms\":" << base_reinsert_ms
                  << ",\"inserted_delete_ms\":" << inserted_delete_ms
                  << ",\"final_mutation_count\":0,\"all_oracles_passed\":true}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
