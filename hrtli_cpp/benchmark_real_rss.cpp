#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <malloc.h>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <unistd.h>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::steady_clock;

#if defined(HRTLI_WITH_HOT)
struct RealPayload {
    const char* key;
    uint64_t value;
};

template <typename ValueType>
struct RealPayloadExtractor {
    using KeyType = const char*;
    KeyType operator()(ValueType const& payload) const { return payload->key; }
    KeyType operator()(KeyType key) const { return key; }
};
#endif

static std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open " + path);
    }
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) keys.push_back(std::move(line));
    }
    return keys;
}

static long rss_kib() {
    std::ifstream status("/proc/self/status");
    std::string field;
    while (status >> field) {
        if (field == "VmRSS:") {
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

static double millis(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static void emit(const std::string& index, size_t initial_keys, size_t insert_keys,
                 double load_ms, double insert_ms, long rss_before_kib,
                 long rss_after_kib, size_t verified, uint64_t sink) {
    std::cout << std::setprecision(10)
              << "{\"index\":\"" << index << "\","
              << "\"initial_keys\":" << initial_keys << ","
              << "\"insert_keys\":" << insert_keys << ","
              << "\"total_keys\":" << initial_keys + insert_keys << ","
              << "\"load_ms\":" << load_ms << ","
              << "\"insert_ms\":" << insert_ms << ","
              << "\"rss_before_kib\":" << rss_before_kib << ","
              << "\"rss_after_kib\":" << rss_after_kib << ","
              << "\"rss_delta_kib\":" << rss_after_kib - rss_before_kib << ","
              << "\"verified_queries\":" << verified << ","
              << "\"sink\":" << sink << "}\n";
}

template <typename Lookup>
static uint64_t verify_queries(const std::vector<std::string>& initial,
                               const std::vector<std::string>& inserts,
                               size_t count, Lookup&& lookup) {
    std::mt19937_64 rng(20260815);
    std::uniform_int_distribution<size_t> choose_initial(0, initial.size() - 1);
    std::uniform_int_distribution<size_t> choose_insert(0, inserts.size() - 1);
    uint64_t sink = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& key = (i & 1) == 0 ? initial[choose_initial(rng)]
                                       : inserts[choose_insert(rng)];
        sink += lookup(key) ? 1 : 0;
    }
    return sink;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0]
                  << " hrtli|art|hot INITIAL_FILE INSERT_FILE [verify_queries]\n";
        return 2;
    }

    const std::string selected = argv[1];
    const std::string initial_path = argv[2];
    const std::string insert_path = argv[3];
    const size_t verify_count = argc > 4 ? std::stoull(argv[4]) : 100000;

    std::vector<std::string> initial;
    std::vector<std::string> inserts;
    try {
        initial = read_keys(initial_path);
        inserts = read_keys(insert_path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (initial.empty() || inserts.empty()) {
        std::cerr << "dataset files must both contain keys\n";
        return 1;
    }

    malloc_trim(0);
    const long before_kib = rss_kib();

    if (selected == "hrtli") {
        const auto load_start = Clock::now();
        hrtli::RankTransportIndex index(initial, 64);
        const auto load_end = Clock::now();
        const auto insert_start = Clock::now();
        for (const auto& key : inserts) index.insert(key);
        const auto insert_end = Clock::now();
        malloc_trim(0);
        const long after_kib = rss_kib();
        const uint64_t sink = verify_queries(initial, inserts, verify_count,
            [&](const std::string& key) { return index.point_lookup(key); });
        emit("HRT-LI", initial.size(), inserts.size(), millis(load_start, load_end),
             millis(insert_start, insert_end), before_kib, after_kib, verify_count, sink);
        return sink == verify_count ? 0 : 3;
    }

#if defined(HRTLI_WITH_ART)
    if (selected == "art") {
        art_tree index;
        art_tree_init(&index);
        const auto load_start = Clock::now();
        for (size_t i = 0; i < initial.size(); ++i) {
            art_insert(&index, reinterpret_cast<const unsigned char*>(initial[i].c_str()),
                       static_cast<int>(initial[i].size()), reinterpret_cast<void*>(i + 1));
        }
        const auto load_end = Clock::now();
        const auto insert_start = Clock::now();
        for (size_t i = 0; i < inserts.size(); ++i) {
            art_insert(&index, reinterpret_cast<const unsigned char*>(inserts[i].c_str()),
                       static_cast<int>(inserts[i].size()),
                       reinterpret_cast<void*>(initial.size() + i + 1));
        }
        const auto insert_end = Clock::now();
        malloc_trim(0);
        const long after_kib = rss_kib();
        const uint64_t sink = verify_queries(initial, inserts, verify_count,
            [&](const std::string& key) {
                return art_search(&index, reinterpret_cast<const unsigned char*>(key.c_str()),
                                  static_cast<int>(key.size())) != nullptr;
            });
        emit("ART", initial.size(), inserts.size(), millis(load_start, load_end),
             millis(insert_start, insert_end), before_kib, after_kib, verify_count, sink);
        art_tree_destroy(&index);
        return sink == verify_count ? 0 : 3;
    }
#endif

#if defined(HRTLI_WITH_HOT)
    if (selected == "hot") {
        hot::singlethreaded::HOTSingleThreaded<RealPayload*, RealPayloadExtractor> index;
        std::vector<std::unique_ptr<RealPayload>> payloads;
        payloads.reserve(initial.size() + inserts.size());
        const auto load_start = Clock::now();
        for (size_t i = 0; i < initial.size(); ++i) {
            payloads.push_back(std::make_unique<RealPayload>(RealPayload{initial[i].c_str(), i + 1}));
            index.insert(payloads.back().get());
        }
        const auto load_end = Clock::now();
        const auto insert_start = Clock::now();
        for (size_t i = 0; i < inserts.size(); ++i) {
            payloads.push_back(std::make_unique<RealPayload>(
                RealPayload{inserts[i].c_str(), initial.size() + i + 1}));
            index.insert(payloads.back().get());
        }
        const auto insert_end = Clock::now();
        malloc_trim(0);
        const long after_kib = rss_kib();
        const uint64_t sink = verify_queries(initial, inserts, verify_count,
            [&](const std::string& key) { return index.lookup(key.c_str()).mIsValid; });
        emit("HOT", initial.size(), inserts.size(), millis(load_start, load_end),
             millis(insert_start, insert_end), before_kib, after_kib, verify_count, sink);
        return sink == verify_count ? 0 : 3;
    }
#endif

    std::cerr << "unsupported index: " << selected << '\n';
    return 2;
}
