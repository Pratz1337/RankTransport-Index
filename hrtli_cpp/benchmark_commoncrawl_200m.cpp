#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "packed_rank_transport.hpp"

using Clock = std::chrono::steady_clock;

static double millis(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static long status_kib(const std::string& wanted) {
    std::ifstream status("/proc/self/status");
    std::string field;
    while (status >> field) {
        if (field == wanted + ":") {
            long value = 0;
            std::string unit;
            status >> value >> unit;
            return value;
        }
        std::string remainder;
        std::getline(status, remainder);
    }
    return -1;
}

static std::vector<std::string> read_keys(const std::string& path,
                                          size_t expected_count) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path);
    std::vector<std::string> keys;
    keys.reserve(expected_count);
    std::string line;
    while (std::getline(input, line)) keys.push_back(line);
    if (keys.size() != expected_count) {
        throw std::runtime_error(path + " has " + std::to_string(keys.size()) +
                                 " keys; expected " + std::to_string(expected_count));
    }
    return keys;
}

static void validate_insert_set(const hrtli::MappedKeyStore& base,
                                const std::vector<std::string>& inserts) {
    if (inserts.empty() || !std::is_sorted(inserts.begin(), inserts.end()) ||
        std::adjacent_find(inserts.begin(), inserts.end()) != inserts.end()) {
        throw std::runtime_error("held-out insert keys are not strictly sorted and unique");
    }
    for (const auto& key : inserts) {
        if (base.contains(key)) {
            throw std::runtime_error("held-out insert key appears in the frozen base");
        }
    }
}

static std::vector<std::string> evenly_spaced_sample(
    const hrtli::MappedKeyStore& keys, size_t wanted) {
    wanted = std::min(wanted, keys.size());
    std::vector<std::string> sample;
    sample.reserve(wanted);
    for (size_t i = 0; i < wanted; ++i) {
        const size_t index = static_cast<size_t>(
            (static_cast<unsigned long long>(i) * keys.size()) / wanted);
        sample.emplace_back(keys.view(index));
    }
    return sample;
}

template <typename Lookup>
static double time_membership(const std::vector<std::string>& base_sample,
                              const std::vector<std::string>& inserts,
                              size_t query_count, Lookup&& lookup) {
    const size_t warmup = std::min<size_t>(10000, query_count);
    for (size_t i = 0; i < warmup; ++i) {
        const std::string& key = (i & 1U) == 0
            ? base_sample[(i / 2) % base_sample.size()]
            : inserts[(i / 2) % inserts.size()];
        if (!lookup(key)) throw std::runtime_error("warm-up membership miss");
    }
    uint64_t hits = 0;
    const auto start = Clock::now();
    for (size_t i = 0; i < query_count; ++i) {
        const std::string& key = (i & 1U) == 0
            ? base_sample[(i / 2) % base_sample.size()]
            : inserts[(i / 2) % inserts.size()];
        hits += lookup(key) ? 1U : 0U;
    }
    const auto end = Clock::now();
    if (hits != query_count) throw std::runtime_error("timed membership miss");
    return millis(start, end);
}

static void emit_result(const std::string& index, size_t initial_count,
                        size_t insert_count, size_t successful_inserts,
                        double input_read_ms, double load_ms, double insert_ms,
                        double membership_ms, size_t query_count,
                        size_t range_queries, double range_ms, uint64_t range_sink,
                        long rss_empty_kib, long rss_inputs_kib, long rss_index_kib,
                        size_t estimated_memory_bytes, const std::string& extra) {
    std::cout << std::setprecision(12)
              << "{\"dataset\":\"Common Crawl cc-main-2026-may-jun-jul hosts\","
              << "\"index\":\"" << index << "\","
              << "\"initial_keys\":" << initial_count << ','
              << "\"insert_keys\":" << insert_count << ','
              << "\"total_keys\":" << initial_count + insert_count << ','
              << "\"successful_inserts\":" << successful_inserts << ','
              << "\"input_read_ms\":" << input_read_ms << ','
              << "\"load_ms\":" << load_ms << ','
              << "\"insert_ms\":" << insert_ms << ','
              << "\"membership_queries\":" << query_count << ','
              << "\"membership_ms\":" << membership_ms << ','
              << "\"membership_mops\":" << query_count / membership_ms / 1000.0 << ','
              << "\"membership_mean_ns\":" << membership_ms * 1.0e6 / query_count << ','
              << "\"range_queries\":" << range_queries << ','
              << "\"range_ms\":" << range_ms << ','
              << "\"range_mean_ns\":"
              << (range_queries == 0 ? 0.0 : range_ms * 1.0e6 / range_queries) << ','
              << "\"range_sink\":" << range_sink << ','
              << "\"rss_empty_kib\":" << rss_empty_kib << ','
              << "\"rss_inputs_kib\":" << rss_inputs_kib << ','
              << "\"rss_index_kib\":" << rss_index_kib << ','
              << "\"peak_rss_kib\":" << status_kib("VmHWM") << ','
              << "\"estimated_memory_bytes\":" << estimated_memory_bytes
              << extra << "}\n";
}

static int run_hrtli(const std::string& initial_path,
                     const std::vector<std::string>& inserts,
                     size_t initial_count, size_t query_count,
                     double input_read_ms, long rss_empty_kib,
                     long rss_inputs_kib) {
    const auto load_start = Clock::now();
    // Preserve the legacy hash/binary configuration explicitly for this driver.
    hrtli::PackedRankTransportIndex index(initial_path, initial_count, 64, true);
    const auto load_end = Clock::now();
    validate_insert_set(index.base_store(), inserts);
    const auto base_sample = evenly_spaced_sample(index.base_store(), 100000);

    size_t successful = 0;
    const auto insert_start = Clock::now();
    for (const auto& key : inserts) successful += index.insert(key) ? 1U : 0U;
    const auto insert_end = Clock::now();
    malloc_trim(0);
    const long rss_index_kib = status_kib("VmRSS");

    const double membership_ms = time_membership(
        base_sample, inserts, query_count,
        [&](const std::string& key) { return index.point_lookup(key); });

    uint64_t range_sink = 0;
    const size_t range_queries = base_sample.size() - 1;
    const auto range_start = Clock::now();
    for (size_t i = 0; i < range_queries; ++i) {
        range_sink += static_cast<uint64_t>(
            index.binary_count_range(base_sample[i], base_sample[i + 1]));
    }
    const auto range_end = Clock::now();

    int transported_sample_max_error = 0;
    uint64_t rank_sink = 0;
    std::vector<int> exact_ranks;
    exact_ranks.reserve(base_sample.size() + inserts.size());
    const auto rank_start = Clock::now();
    for (const auto& key : base_sample) {
        const int exact = index.binary_exact_rank(key);
        exact_ranks.push_back(exact);
        rank_sink += static_cast<uint64_t>(exact);
    }
    for (const auto& key : inserts) {
        const int exact = index.binary_exact_rank(key);
        exact_ranks.push_back(exact);
        rank_sink += static_cast<uint64_t>(exact);
    }
    const auto rank_end = Clock::now();
    size_t result_index = 0;
    for (const auto& key : base_sample) {
        const int expected = static_cast<int>(
            index.base_store().lower_bound(key) +
            static_cast<size_t>(std::lower_bound(inserts.begin(), inserts.end(), key) -
                                inserts.begin()));
        if (exact_ranks[result_index] != expected) {
            throw std::runtime_error("HRT-LI base-key exact rank disagrees with oracle");
        }
        transported_sample_max_error = std::max(
            transported_sample_max_error,
            std::abs(index.transported_predict(key) - exact_ranks[result_index]));
        ++result_index;
    }
    for (const auto& key : inserts) {
        const int expected = static_cast<int>(
            index.base_store().lower_bound(key) +
            static_cast<size_t>(std::lower_bound(inserts.begin(), inserts.end(), key) -
                                inserts.begin()));
        if (exact_ranks[result_index++] != expected) {
            throw std::runtime_error("HRT-LI inserted-key exact rank disagrees with oracle");
        }
    }
    const int base_model_max_error = index.base_model_max_error();

    const std::string extra =
        ",\"model_segments\":" + std::to_string(index.model_segments()) +
        ",\"certified_epsilon\":64" +
        ",\"base_model_max_error\":" + std::to_string(base_model_max_error) +
        ",\"transported_sample_keys\":" + std::to_string(base_sample.size()) +
        ",\"transported_sample_max_error\":" +
            std::to_string(transported_sample_max_error) +
        ",\"fingerprint_capacity\":" +
            std::to_string(index.fingerprint_capacity()) +
        ",\"fingerprint_average_build_probes\":" +
            std::to_string(index.average_build_probes()) +
        ",\"exact_rank_queries\":" +
            std::to_string(base_sample.size() + inserts.size()) +
        ",\"exact_rank_ms\":" + std::to_string(millis(rank_start, rank_end)) +
        ",\"rank_sink\":" + std::to_string(rank_sink);
    for (size_t i = 0; i < range_queries; ++i) {
        const auto& lo = base_sample[i];
        const auto& hi = base_sample[i + 1];
        const size_t expected = index.base_store().upper_bound(hi) - index.base_store().lower_bound(lo) +
            (std::upper_bound(inserts.begin(), inserts.end(), hi) -
             std::lower_bound(inserts.begin(), inserts.end(), lo));
        if (index.binary_count_range(lo, hi) != static_cast<int>(expected)) {
            throw std::runtime_error("HRT-LI range disagrees with oracle");
        }
    }
    if (successful != inserts.size() || base_model_max_error > 64 || transported_sample_max_error > 64) {
        throw std::runtime_error("HRT-LI correctness gate failed");
    }
    emit_result("HRT-LI-packed", initial_count, inserts.size(), successful,
                input_read_ms, millis(load_start, load_end),
                millis(insert_start, insert_end), membership_ms, query_count,
                range_queries, millis(range_start, range_end), range_sink,
                rss_empty_kib, rss_inputs_kib, rss_index_kib,
                index.estimated_memory_bytes(), extra);
    return successful == inserts.size() && base_model_max_error <= 64 &&
                   transported_sample_max_error <= 64
        ? 0 : 3;
}

static int run_sorted(const std::string& initial_path,
                      const std::vector<std::string>& inserts,
                      size_t initial_count, size_t query_count,
                      double input_read_ms, long rss_empty_kib,
                      long rss_inputs_kib) {
    const auto load_start = Clock::now();
    hrtli::MappedKeyStore base(initial_path, initial_count);
    if (!base.strictly_sorted_unique()) {
        throw std::runtime_error("mapped base keys are not strictly sorted and unique");
    }
    base.advise_random();
    std::vector<std::string> delta;
    const auto load_end = Clock::now();
    validate_insert_set(base, inserts);
    const auto base_sample = evenly_spaced_sample(base, 100000);

    const auto insert_start = Clock::now();
    size_t successful_inserts = 0;
    for (const auto& key : inserts) {
        if (base.contains(key)) continue;
        const auto pos = std::lower_bound(delta.begin(), delta.end(), key);
        if (pos == delta.end() || *pos != key) {
            delta.insert(pos, key);
            ++successful_inserts;
        }
    }
    const auto insert_end = Clock::now();
    malloc_trim(0);
    const long rss_index_kib = status_kib("VmRSS");
    const double membership_ms = time_membership(
        base_sample, inserts, query_count,
        [&](const std::string& key) {
            return base.contains(key) || std::binary_search(delta.begin(), delta.end(), key);
        });

    uint64_t range_sink = 0;
    const size_t range_queries = base_sample.size() - 1;
    const auto range_start = Clock::now();
    for (size_t i = 0; i < range_queries; ++i) {
        range_sink += static_cast<uint64_t>(
            base.upper_bound(base_sample[i + 1]) - base.lower_bound(base_sample[i]) +
            std::upper_bound(delta.begin(), delta.end(), base_sample[i + 1]) -
            std::lower_bound(delta.begin(), delta.end(), base_sample[i]));
    }
    const auto range_end = Clock::now();

    uint64_t rank_sink = 0;
    std::vector<int> exact_ranks;
    exact_ranks.reserve(base_sample.size() + inserts.size());
    const auto rank_start = Clock::now();
    for (const auto& key : base_sample) {
        const size_t exact = base.lower_bound(key) + static_cast<size_t>(
            std::lower_bound(delta.begin(), delta.end(), key) - delta.begin());
        exact_ranks.push_back(static_cast<int>(exact));
        rank_sink += exact;
    }
    for (const auto& key : inserts) {
        const size_t exact = base.lower_bound(key) + static_cast<size_t>(
            std::lower_bound(delta.begin(), delta.end(), key) - delta.begin());
        exact_ranks.push_back(static_cast<int>(exact));
        rank_sink += exact;
    }
    const auto rank_end = Clock::now();
    for (size_t i = 0; i < base_sample.size(); ++i) {
        const size_t base_position = i * base.size() / base_sample.size();
        const size_t inserted_less = std::lower_bound(inserts.begin(), inserts.end(), base_sample[i]) - inserts.begin();
        if (exact_ranks[i] != static_cast<int>(base_position + inserted_less)) {
            throw std::runtime_error("sorted base rank disagrees with sample position oracle");
        }
    }
    for (size_t i = 0; i < inserts.size(); ++i) {
        if (exact_ranks[base_sample.size() + i] != static_cast<int>(base.lower_bound(inserts[i]) + i)) {
            throw std::runtime_error("sorted inserted rank disagrees with oracle");
        }
    }
    if (successful_inserts != inserts.size()) throw std::runtime_error("sorted insertion gate failed");
    size_t delta_bytes = delta.capacity() * sizeof(std::string);
    for (const auto& key : delta) delta_bytes += key.capacity();
    const std::string extra =
        ",\"exact_rank_queries\":" +
            std::to_string(base_sample.size() + inserts.size()) +
        ",\"exact_rank_ms\":" + std::to_string(millis(rank_start, rank_end)) +
        ",\"rank_sink\":" + std::to_string(rank_sink);
    emit_result("mapped-sorted-plus-delta", initial_count, inserts.size(),
                successful_inserts, input_read_ms, millis(load_start, load_end),
                millis(insert_start, insert_end), membership_ms, query_count,
                range_queries, millis(range_start, range_end), range_sink,
                rss_empty_kib, rss_inputs_kib, rss_index_kib,
                base.mapped_bytes() + base.offsets_bytes() + delta_bytes, extra);
    return 0;
}

#if defined(HRTLI_WITH_ART)
static int run_art(const std::string& initial_path,
                   const std::vector<std::string>& inserts,
                   size_t initial_count, size_t query_count,
                   double input_read_ms, long rss_empty_kib,
                   long rss_inputs_kib) {
    art_tree index;
    art_tree_init(&index);
    std::ifstream input(initial_path);
    if (!input) throw std::runtime_error("cannot open " + initial_path);
    const size_t sample_count = std::min<size_t>(100000, initial_count);
    const size_t sample_stride = std::max<size_t>(1, initial_count / sample_count);
    std::vector<std::string> base_sample;
    base_sample.reserve(sample_count);
    std::string key;
    size_t count = 0;
    const auto load_start = Clock::now();
    while (std::getline(input, key)) {
        if (base_sample.size() < sample_count && count % sample_stride == 0) {
            base_sample.push_back(key);
        }
        art_insert(&index, reinterpret_cast<const unsigned char*>(key.data()),
                   static_cast<int>(key.size()),
                   reinterpret_cast<void*>(static_cast<uintptr_t>(count + 1)));
        ++count;
    }
    const auto load_end = Clock::now();
    if (count != initial_count) throw std::runtime_error("ART input count mismatch");
    const auto insert_start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        art_insert(&index,
                   reinterpret_cast<const unsigned char*>(inserts[i].data()),
                   static_cast<int>(inserts[i].size()),
                   reinterpret_cast<void*>(static_cast<uintptr_t>(initial_count + i + 1)));
    }
    const auto insert_end = Clock::now();
    malloc_trim(0);
    const long rss_index_kib = status_kib("VmRSS");
    const double membership_ms = time_membership(
        base_sample, inserts, query_count,
        [&](const std::string& candidate) {
            return art_search(&index,
                reinterpret_cast<const unsigned char*>(candidate.data()),
                static_cast<int>(candidate.size())) != nullptr;
        });
    emit_result("ART", initial_count, inserts.size(), inserts.size(),
                input_read_ms, millis(load_start, load_end),
                millis(insert_start, insert_end), membership_ms, query_count,
                0, 0.0, 0, rss_empty_kib, rss_inputs_kib, rss_index_kib, 0, "");
    art_tree_destroy(&index);
    return 0;
}
#endif

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "Usage: " << argv[0]
                  << " hrtli|sorted|art INITIAL INSERT INITIAL_COUNT INSERT_COUNT"
                     " [QUERY_COUNT]\n";
        return 2;
    }
    try {
        const std::string selected = argv[1];
        const std::string initial_path = argv[2];
        const std::string insert_path = argv[3];
        const size_t initial_count = std::stoull(argv[4]);
        const size_t insert_count = std::stoull(argv[5]);
        const size_t query_count = argc > 6 ? std::stoull(argv[6]) : 1000000;
        if (initial_count + insert_count < 200000000 &&
            std::getenv("HRTLI_SCALE_SMOKE") == nullptr) {
            throw std::runtime_error("corpus must contain at least 200,000,000 real keys");
        }
        malloc_trim(0);
        const long rss_empty_kib = status_kib("VmRSS");
        const auto input_start = Clock::now();
        const std::vector<std::string> inserts = read_keys(insert_path, insert_count);
        const auto input_end = Clock::now();
        const long rss_inputs_kib = status_kib("VmRSS");
        const double input_read_ms = millis(input_start, input_end);

        if (selected == "hrtli") {
            return run_hrtli(initial_path, inserts, initial_count, query_count,
                             input_read_ms, rss_empty_kib, rss_inputs_kib);
        }
        if (selected == "sorted") {
            return run_sorted(initial_path, inserts, initial_count, query_count,
                              input_read_ms, rss_empty_kib, rss_inputs_kib);
        }
#if defined(HRTLI_WITH_ART)
        if (selected == "art") {
            return run_art(initial_path, inserts, initial_count, query_count,
                           input_read_ms, rss_empty_kib, rss_inputs_kib);
        }
#endif
        throw std::runtime_error("unsupported index: " + selected);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
