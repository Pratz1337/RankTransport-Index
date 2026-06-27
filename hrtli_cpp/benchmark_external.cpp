/*
 * External baseline benchmarking harness for HRT-LI.
 *
 * Compiles against the following pinned competitor versions:
 * - ALEX (MIT License): commit 4370da6aa8b509fdc9b0d2c49faa0624b0078589 (https://github.com/microsoft/ALEX.git)
 * - LIPP (MIT License): commit fe6ca4954f00875482f9e4dd63b34dae2384d23b (https://github.com/Jiacheng-WU/LIPP.git)
 * - PGM-index (Apache 2.0 License): commit c6fcf3d34e55eb0061b01e2f49dfcbdb711f1407 (https://github.com/gvinciguerra/PGM-index.git)
 * - libart (BSD 3-Clause License): commit 301046804af165269e37da6725f5a4aec9ecc881 (https://github.com/armon/libart.git)
 * - LITS: commit c9026ac9645b4af1f6e3cb27b42351220f4376d4 (https://github.com/schencoding/lits.git), no root LICENSE found
 * - HOT (ISC License): commit 96bf6fb7103b27e50e16a6026db8974c090ee84a (https://github.com/speedskater/hot.git)
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::high_resolution_clock;

struct Result {
    std::string name;
    double load_ms = 0.0;
    double insert_ms = 0.0;
    double lookup_mops = 0.0;
    double latency_ns = 0.0;
};

#if defined(HRTLI_WITH_HOT)
struct StringPayload {
    const char* key;
    uint64_t value;
};

template <typename ValueType>
struct StringPayloadExtractor {
    using KeyType = const char*;

    KeyType operator()(ValueType const& payload) const {
        return payload->key;
    }

    KeyType operator()(KeyType key) const {
        return key;
    }
};
#endif

std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            keys.push_back(line);
        }
    }
    return keys;
}

std::vector<std::string> sorted_unique(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

double ms_since(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void fill_lookup(Result& result, size_t count, double lookup_ms) {
    result.lookup_mops = (static_cast<double>(count) / (lookup_ms / 1000.0)) / 1e6;
    result.latency_ns = (lookup_ms * 1e6) / static_cast<double>(count);
}

std::unordered_map<std::string, double> build_surrogate_keys(
    const std::vector<std::string>& all_keys) {
    std::unordered_map<std::string, double> out;
    out.reserve(all_keys.size());
    for (size_t i = 0; i < all_keys.size(); ++i) {
        out.emplace(all_keys[i], static_cast<double>(i + 1));
    }
    return out;
}

Result bench_hrtli(const std::vector<std::string>& initial,
                   const std::vector<std::string>& inserts,
                   const std::vector<std::string>& lookups) {
    Result result{"HRT-LI native"};
    auto start = Clock::now();
    hrtli::RankTransportIndex index(initial, 64);
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        index.insert(key);
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile int64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        sink += (index.point_lookup(key) ? 1 : 0);
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}

#if defined(HRTLI_WITH_ALEX)
Result bench_alex(const std::vector<std::string>& initial,
                  const std::vector<std::string>& inserts,
                  const std::vector<std::string>& lookups,
                  const std::unordered_map<std::string, double>& surrogate) {
    Result result{"ALEX numeric surrogate"};
    std::vector<std::pair<double, uint64_t>> bulk;
    bulk.reserve(initial.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        bulk.emplace_back(surrogate.at(initial[i]), i + 1);
    }
    std::sort(bulk.begin(), bulk.end());

    hrtli::external::AlexIndex<double, uint64_t> index;
    auto start = Clock::now();
    index.bulk_load(bulk.data(), static_cast<int>(bulk.size()));
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        index.insert(surrogate.at(inserts[i]), initial.size() + i + 1);
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto it = index.find(surrogate.at(key));
        if (it != index.end()) {
            sink += it.payload();
        }
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}
#endif

#if defined(HRTLI_WITH_LIPP)
Result bench_lipp(const std::vector<std::string>& initial,
                  const std::vector<std::string>& inserts,
                  const std::vector<std::string>& lookups,
                  const std::unordered_map<std::string, double>& surrogate) {
    Result result{"LIPP numeric surrogate"};
    std::vector<std::pair<double, uint64_t>> bulk;
    bulk.reserve(initial.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        bulk.emplace_back(surrogate.at(initial[i]), i + 1);
    }
    std::sort(bulk.begin(), bulk.end());

    hrtli::external::LippIndex<double, uint64_t> index;
    auto start = Clock::now();
    index.bulk_load(bulk.data(), static_cast<int>(bulk.size()));
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        index.insert(surrogate.at(inserts[i]), initial.size() + i + 1);
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        double s = surrogate.at(key);
        if (index.exists(s)) {
            sink += index.at(s);
        }
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}
#endif

#if defined(HRTLI_WITH_PGM)
Result bench_pgm(const std::vector<std::string>& initial,
                 const std::vector<std::string>& inserts,
                 const std::vector<std::string>& lookups,
                 const std::unordered_map<std::string, double>& surrogate) {
    Result result{"PGM dynamic surrogate"};
    std::vector<std::pair<double, uint64_t>> bulk;
    bulk.reserve(initial.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        bulk.emplace_back(surrogate.at(initial[i]), i + 1);
    }
    std::sort(bulk.begin(), bulk.end());

    auto start = Clock::now();
    hrtli::external::DynamicPgmIndex<double, uint64_t> index(bulk.begin(), bulk.end());
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        index.insert_or_assign(surrogate.at(inserts[i]), initial.size() + i + 1);
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto it = index.find(surrogate.at(key));
        if (it != index.end()) {
            sink += it->second;
        }
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}
#endif

#if defined(HRTLI_WITH_ART)
Result bench_art(const std::vector<std::string>& initial,
                 const std::vector<std::string>& inserts,
                 const std::vector<std::string>& lookups) {
    Result result{"ART string"};
    art_tree index;
    art_tree_init(&index);
    auto start = Clock::now();
    for (size_t i = 0; i < initial.size(); ++i) {
        art_insert(&index, reinterpret_cast<const unsigned char*>(initial[i].c_str()),
                   static_cast<int>(initial[i].size()), reinterpret_cast<void*>(i + 1));
    }
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        art_insert(&index, reinterpret_cast<const unsigned char*>(inserts[i].c_str()),
                   static_cast<int>(inserts[i].size()),
                   reinterpret_cast<void*>(initial.size() + i + 1));
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uintptr_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        sink += reinterpret_cast<uintptr_t>(
            art_search(&index, reinterpret_cast<const unsigned char*>(key.c_str()),
                       static_cast<int>(key.size())));
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    art_tree_destroy(&index);
    (void)sink;
    return result;
}
#endif

#if defined(HRTLI_WITH_LITS)
Result bench_lits(const std::vector<std::string>& initial,
                  const std::vector<std::string>& inserts,
                  const std::vector<std::string>& lookups) {
    Result result{"LITS string"};
    if (initial.size() < 1000) {
        result.name = "LITS string (skipped <1000)";
        return result;
    }
    std::vector<const char*> keys;
    std::vector<uint64_t> vals;
    keys.reserve(initial.size());
    vals.reserve(initial.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        keys.push_back(initial[i].c_str());
        vals.push_back(i + 1);
    }

    lits::LITS index;
    auto start = Clock::now();
    index.bulkload(keys.data(), vals.data(), static_cast<int>(keys.size()));
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        index.insert(inserts[i].c_str(), initial.size() + i + 1);
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto found = index.lookup(key.c_str());
        if (found) {
            sink += found->read();
        }
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}
#endif

#if defined(HRTLI_WITH_HOT)
Result bench_hot(const std::vector<std::string>& initial,
                 const std::vector<std::string>& inserts,
                 const std::vector<std::string>& lookups) {
    Result result{"HOT string"};
    hot::singlethreaded::HOTSingleThreaded<StringPayload*, StringPayloadExtractor> index;
    std::vector<std::unique_ptr<StringPayload>> payloads;
    payloads.reserve(initial.size() + inserts.size());

    auto start = Clock::now();
    for (size_t i = 0; i < initial.size(); ++i) {
        payloads.push_back(std::make_unique<StringPayload>(StringPayload{initial[i].c_str(), i + 1}));
        index.insert(payloads.back().get());
    }
    auto end = Clock::now();
    result.load_ms = ms_since(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        payloads.push_back(std::make_unique<StringPayload>(
            StringPayload{inserts[i].c_str(), initial.size() + i + 1}));
        index.insert(payloads.back().get());
    }
    end = Clock::now();
    result.insert_ms = ms_since(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto found = index.lookup(key.c_str());
        if (found.mIsValid) {
            sink += found.mValue->value;
        }
    }
    end = Clock::now();
    fill_lookup(result, lookups.size(), ms_since(start, end));
    (void)sink;
    return result;
}
#endif

#include <sstream>

struct Stats {
    std::vector<double> values;
    double mean() const {
        double sum = 0.0;
        for (double v : values) sum += v;
        return values.empty() ? 0.0 : sum / values.size();
    }
    double stddev() const {
        double m = mean();
        double sum = 0.0;
        for (double v : values) sum += (v - m) * (v - m);
        return values.size() <= 1 ? 0.0 : std::sqrt(sum / (values.size() - 1));
    }
    double ci95() const {
        return values.empty() ? 0.0 : 1.96 * stddev() / std::sqrt(values.size());
    }
};

struct TrialResult {
    std::string name;
    Stats load_ms;
    Stats insert_ms;
    Stats lookup_mops;
    Stats latency_ns;
};

std::string format_stat(const Stats& s) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3) << s.mean() << " +/- " << std::setprecision(3) << s.ci95();
    return ss.str();
}

void print_trial_results(const std::string& dataset, const std::vector<TrialResult>& results) {
    std::cout << "\nExternal benchmark dataset: " << dataset << " (Repeated Trials with 95% CI)\n";
    std::cout << std::left << std::setw(30) << "Index"
              << std::right << std::setw(22) << "Load ms"
              << std::setw(22) << "Insert ms"
              << std::setw(22) << "Lookup Mops"
              << std::setw(22) << "Latency ns" << "\n";
    std::cout << std::string(118, '-') << "\n";
    for (const auto& res : results) {
        if (res.load_ms.values.empty() || res.name.find("skipped") != std::string::npos) {
            std::cout << std::left << std::setw(30) << res.name
                      << std::right << std::setw(22) << "skipped"
                      << std::setw(22) << "skipped"
                      << std::setw(22) << "skipped"
                      << std::setw(22) << "skipped" << "\n";
        } else {
            std::cout << std::left << std::setw(30) << res.name
                      << std::right << std::setw(22) << format_stat(res.load_ms)
                      << std::setw(22) << format_stat(res.insert_ms)
                      << std::setw(22) << format_stat(res.lookup_mops)
                      << std::setw(22) << format_stat(res.latency_ns) << "\n";
        }
    }
}

std::string json_escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void write_trial_results_json(const std::string& dataset, const std::vector<TrialResult>& results) {
    std::ofstream out("results_q1/external_benchmark_" + dataset + ".json");
    if (!out) {
        return;
    }
    out << "{\n  \"dataset\": \"" << json_escape(dataset) << "\",\n  \"results\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& res = results[i];
        out << "    {\n";
        out << "      \"name\": \"" << json_escape(res.name) << "\",\n";
        out << "      \"trials\": " << res.load_ms.values.size() << ",\n";
        out << "      \"load_ms_mean\": " << res.load_ms.mean() << ",\n";
        out << "      \"load_ms_ci95\": " << res.load_ms.ci95() << ",\n";
        out << "      \"insert_ms_mean\": " << res.insert_ms.mean() << ",\n";
        out << "      \"insert_ms_ci95\": " << res.insert_ms.ci95() << ",\n";
        out << "      \"lookup_mops_mean\": " << res.lookup_mops.mean() << ",\n";
        out << "      \"lookup_mops_ci95\": " << res.lookup_mops.ci95() << ",\n";
        out << "      \"latency_ns_mean\": " << res.latency_ns.mean() << ",\n";
        out << "      \"latency_ns_ci95\": " << res.latency_ns.ci95() << "\n";
        out << "    }" << (i + 1 == results.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

void append_result(TrialResult& trial, const Result& result) {
    trial.name = result.name;
    if (result.name.find("skipped") != std::string::npos) {
        return;
    }
    trial.load_ms.values.push_back(result.load_ms);
    trial.insert_ms.values.push_back(result.insert_ms);
    trial.lookup_mops.values.push_back(result.lookup_mops);
    trial.latency_ns.values.push_back(result.latency_ns);
}

int parse_trials(int argc, char** argv) {
    if (argc > 2) {
        return std::max(1, std::atoi(argv[2]));
    }
    const char* env_trials = std::getenv("HRTLI_EXTERNAL_TRIALS");
    return env_trials == nullptr ? 5 : std::max(1, std::atoi(env_trials));
}

int main(int argc, char** argv) {
    std::string dataset = argc > 1 ? argv[1] : "url";
    auto initial = read_keys("data/" + dataset + "_initial.txt");
    auto inserts = read_keys("data/" + dataset + "_insert.txt");
    if (initial.empty() || inserts.empty()) {
        std::cerr << "Missing or empty dataset files for " << dataset << "\n";
        return 1;
    }

    std::vector<std::string> lookups = initial;
    lookups.insert(lookups.end(), inserts.begin(), inserts.end());
    lookups = sorted_unique(std::move(lookups));
    std::mt19937 rng(2026);
    std::shuffle(lookups.begin(), lookups.end(), rng);
    auto surrogate = build_surrogate_keys(lookups);

    int num_trials = parse_trials(argc, argv);
    std::vector<TrialResult> results;
    results.push_back(TrialResult{"HRT-LI native"});
    const size_t hrtli_idx = results.size() - 1;
#if defined(HRTLI_WITH_ALEX)
    results.push_back(TrialResult{"ALEX numeric surrogate"});
    const size_t alex_idx = results.size() - 1;
#endif
#if defined(HRTLI_WITH_LIPP)
    results.push_back(TrialResult{"LIPP numeric surrogate"});
    const size_t lipp_idx = results.size() - 1;
#endif
#if defined(HRTLI_WITH_PGM)
    results.push_back(TrialResult{"PGM dynamic surrogate"});
    const size_t pgm_idx = results.size() - 1;
#endif
#if defined(HRTLI_WITH_ART)
    results.push_back(TrialResult{"ART string"});
    const size_t art_idx = results.size() - 1;
#endif
#if defined(HRTLI_WITH_LITS)
    results.push_back(TrialResult{"LITS string"});
    const size_t lits_idx = results.size() - 1;
#endif
#if defined(HRTLI_WITH_HOT)
    results.push_back(TrialResult{"HOT string"});
    const size_t hot_idx = results.size() - 1;
#endif

    for (int t = 0; t < num_trials; ++t) {
        std::cout << "Running trial " << (t + 1) << "/" << num_trials << " for dataset: " << dataset << "..." << std::endl;
        
        append_result(results[hrtli_idx], bench_hrtli(initial, inserts, lookups));
#if defined(HRTLI_WITH_ALEX)
        append_result(results[alex_idx], bench_alex(initial, inserts, lookups, surrogate));
#endif
#if defined(HRTLI_WITH_LIPP)
        append_result(results[lipp_idx], bench_lipp(initial, inserts, lookups, surrogate));
#endif
#if defined(HRTLI_WITH_PGM)
        append_result(results[pgm_idx], bench_pgm(initial, inserts, lookups, surrogate));
#endif
#if defined(HRTLI_WITH_ART)
        append_result(results[art_idx], bench_art(initial, inserts, lookups));
#endif
#if defined(HRTLI_WITH_LITS)
        append_result(results[lits_idx], bench_lits(initial, inserts, lookups));
#endif
#if defined(HRTLI_WITH_HOT)
        append_result(results[hot_idx], bench_hot(initial, inserts, lookups));
#endif
    }

    print_trial_results(dataset, results);
    const char* write_json = std::getenv("HRTLI_EXTERNAL_WRITE_JSON");
    if (write_json != nullptr && std::string(write_json) == "1") {
        write_trial_results_json(dataset, results);
    }
    return 0;
}
