#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::steady_clock;

#if defined(HRTLI_WITH_HOT)
struct MillionPayload {
    const char* key;
    uint64_t value;
};

template <typename ValueType>
struct MillionPayloadExtractor {
    using KeyType = const char*;
    KeyType operator()(ValueType const& payload) const { return payload->key; }
    KeyType operator()(KeyType key) const { return key; }
};
#endif

static std::string pad(size_t value, int width) {
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << value;
    return out.str();
}

static std::string make_key(size_t value) {
    return "/tenant/" + pad(value % 4096, 4) +
           "/region/" + pad((value / 4096) % 64, 2) +
           "/service/" + pad((value / 262144) % 64, 2) +
           "/object/" + pad(value, 7);
}

static long rss_kib() {
    std::ifstream statm("/proc/self/statm");
    long total_pages = 0;
    long resident_pages = 0;
    statm >> total_pages >> resident_pages;
    (void)total_pages;
    return resident_pages * sysconf(_SC_PAGESIZE) / 1024;
}

static double millis(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

static void emit(const std::string& index, size_t total_keys, size_t initial_keys,
                 size_t insert_keys, size_t lookup_count, double load_ms,
                 double insert_ms, double lookup_ms, long rss_delta_kib,
                 uint64_t sink) {
    double mops = static_cast<double>(lookup_count) / (lookup_ms / 1000.0) / 1e6;
    std::cout << std::setprecision(10)
              << "{\"index\":\"" << index << "\","
              << "\"total_keys\":" << total_keys << ","
              << "\"initial_keys\":" << initial_keys << ","
              << "\"insert_keys\":" << insert_keys << ","
              << "\"lookup_count\":" << lookup_count << ","
              << "\"load_ms\":" << load_ms << ","
              << "\"insert_ms\":" << insert_ms << ","
              << "\"lookup_mops\":" << mops << ","
              << "\"latency_ns\":" << lookup_ms * 1e6 / lookup_count << ","
              << "\"rss_delta_kib\":" << rss_delta_kib << ","
              << "\"sink\":" << sink << "}\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " hrtli|art|hot [total_keys] [lookup_count]\n";
        return 2;
    }
    std::string selected = argv[1];
    size_t total_keys = argc > 2 ? std::stoull(argv[2]) : 1000000;
    size_t lookup_count = argc > 3 ? std::stoull(argv[3]) : 1000000;

    std::vector<std::string> initial;
    std::vector<std::string> inserts;
    initial.reserve(total_keys * 4 / 5);
    inserts.reserve(total_keys / 5 + 1);
    for (size_t i = 0; i < total_keys; ++i) {
        if (i % 5 == 0) inserts.push_back(make_key(i));
        else initial.push_back(make_key(i));
    }
    std::sort(initial.begin(), initial.end());
    std::sort(inserts.begin(), inserts.end());

    std::vector<const std::string*> lookups;
    lookups.reserve(lookup_count);
    std::mt19937_64 rng(20260814);
    std::uniform_int_distribution<size_t> choose_all(0, total_keys - 1);
    std::uniform_int_distribution<size_t> choose_initial(0, initial.size() - 1);
    std::uniform_int_distribution<size_t> choose_insert(0, inserts.size() - 1);
    for (size_t i = 0; i < lookup_count; ++i) {
        if (choose_all(rng) % 5 == 0) lookups.push_back(&inserts[choose_insert(rng)]);
        else lookups.push_back(&initial[choose_initial(rng)]);
    }

    long before_kib = rss_kib();
    double load_ms = 0.0;
    double insert_ms = 0.0;
    double lookup_ms = 0.0;
    uint64_t sink = 0;

    if (selected == "hrtli") {
        auto load_start = Clock::now();
        hrtli::RankTransportIndex index(initial, 64);
        auto load_end = Clock::now();
        auto insert_start = Clock::now();
        for (const auto& key : inserts) index.insert(key);
        auto insert_end = Clock::now();
        long after_kib = rss_kib();
        auto lookup_start = Clock::now();
        for (const std::string* key : lookups) sink += index.point_lookup(*key) ? 1 : 0;
        auto lookup_end = Clock::now();
        load_ms = millis(load_start, load_end);
        insert_ms = millis(insert_start, insert_end);
        lookup_ms = millis(lookup_start, lookup_end);
        emit("HRT-LI", total_keys, initial.size(), inserts.size(), lookup_count,
             load_ms, insert_ms, lookup_ms, after_kib - before_kib, sink);
        return sink == lookup_count ? 0 : 3;
    }

#if defined(HRTLI_WITH_ART)
    if (selected == "art") {
        art_tree index;
        art_tree_init(&index);
        auto load_start = Clock::now();
        for (size_t i = 0; i < initial.size(); ++i) {
            art_insert(&index, reinterpret_cast<const unsigned char*>(initial[i].c_str()),
                       static_cast<int>(initial[i].size()), reinterpret_cast<void*>(i + 1));
        }
        auto load_end = Clock::now();
        auto insert_start = Clock::now();
        for (size_t i = 0; i < inserts.size(); ++i) {
            art_insert(&index, reinterpret_cast<const unsigned char*>(inserts[i].c_str()),
                       static_cast<int>(inserts[i].size()),
                       reinterpret_cast<void*>(initial.size() + i + 1));
        }
        auto insert_end = Clock::now();
        long after_kib = rss_kib();
        auto lookup_start = Clock::now();
        for (const std::string* key : lookups) {
            sink += art_search(&index, reinterpret_cast<const unsigned char*>(key->c_str()),
                               static_cast<int>(key->size())) != nullptr;
        }
        auto lookup_end = Clock::now();
        emit("ART", total_keys, initial.size(), inserts.size(), lookup_count,
             millis(load_start, load_end), millis(insert_start, insert_end),
             millis(lookup_start, lookup_end), after_kib - before_kib, sink);
        art_tree_destroy(&index);
        return sink == lookup_count ? 0 : 3;
    }
#endif

#if defined(HRTLI_WITH_HOT)
    if (selected == "hot") {
        hot::singlethreaded::HOTSingleThreaded<MillionPayload*, MillionPayloadExtractor> index;
        std::vector<std::unique_ptr<MillionPayload>> payloads;
        payloads.reserve(total_keys);
        auto load_start = Clock::now();
        for (size_t i = 0; i < initial.size(); ++i) {
            payloads.push_back(std::make_unique<MillionPayload>(MillionPayload{initial[i].c_str(), i + 1}));
            index.insert(payloads.back().get());
        }
        auto load_end = Clock::now();
        auto insert_start = Clock::now();
        for (size_t i = 0; i < inserts.size(); ++i) {
            payloads.push_back(std::make_unique<MillionPayload>(MillionPayload{inserts[i].c_str(), initial.size() + i + 1}));
            index.insert(payloads.back().get());
        }
        auto insert_end = Clock::now();
        long after_kib = rss_kib();
        auto lookup_start = Clock::now();
        for (const std::string* key : lookups) {
            auto found = index.lookup(key->c_str());
            sink += found.mIsValid ? 1 : 0;
        }
        auto lookup_end = Clock::now();
        emit("HOT", total_keys, initial.size(), inserts.size(), lookup_count,
             millis(load_start, load_end), millis(insert_start, insert_end),
             millis(lookup_start, lookup_end), after_kib - before_kib, sink);
        return sink == lookup_count ? 0 : 3;
    }
#endif

    std::cerr << "Unsupported index: " << selected << '\n';
    return 2;
}
