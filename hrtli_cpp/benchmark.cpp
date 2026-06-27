#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::high_resolution_clock;

struct BenchmarkResult {
    std::string index_name;
    double load_time_ms = 0.0;
    double insert_time_ms = 0.0;
    double lookup_throughput_mops = 0.0;
    double avg_lookup_latency_ns = 0.0;
    size_t memory_bytes = 0;
};

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

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void fill_lookup_stats(BenchmarkResult& result, size_t n_lookup, double lookup_ms) {
    result.lookup_throughput_mops = (static_cast<double>(n_lookup) / (lookup_ms / 1000.0)) / 1e6;
    result.avg_lookup_latency_ns = (lookup_ms * 1e6) / static_cast<double>(n_lookup);
}

BenchmarkResult bench_hrtli(const std::vector<std::string>& initial,
                            const std::vector<std::string>& inserts,
                            const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "HRT-LI native rank transport";

    auto start = Clock::now();
    hrtli::RankTransportIndex index(initial, 64);
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        index.insert(key);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        sink += (index.point_lookup(key) ? 1 : 0);
    }
    end = Clock::now();
    fill_lookup_stats(result, lookups.size(), elapsed_ms(start, end));
    result.memory_bytes = index.memory_bytes();
    (void)sink;
    return result;
}

BenchmarkResult bench_hrtli_v3(const std::vector<std::string>& initial,
                               const std::vector<std::string>& inserts,
                               const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "HRT-LI v3";

    auto start = Clock::now();
    hrtli::RankTransportIndexV3 index(initial, 64);
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        index.insert(key);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        sink += (index.point_lookup(key) ? 1 : 0);
    }
    end = Clock::now();
    fill_lookup_stats(result, lookups.size(), elapsed_ms(start, end));
    result.memory_bytes = index.memory_bytes();
    (void)sink;
    return result;
}

BenchmarkResult bench_sorted_vector(const std::vector<std::string>& initial,
                                    const std::vector<std::string>& inserts,
                                    const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "sorted vector lower_bound";

    auto start = Clock::now();
    std::vector<std::string> keys = sorted_unique(initial);
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        if (it == keys.end() || *it != key) {
            keys.insert(it, key);
        }
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        sink += ((it != keys.end() && *it == key) ? 1 : 0);
    }
    end = Clock::now();
    fill_lookup_stats(result, lookups.size(), elapsed_ms(start, end));
    result.memory_bytes = keys.size() * sizeof(std::string);
    for (const auto& key : keys) {
        result.memory_bytes += key.capacity();
    }
    (void)sink;
    return result;
}

BenchmarkResult bench_std_set(const std::vector<std::string>& initial,
                              const std::vector<std::string>& inserts,
                              const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "std::set";

    auto start = Clock::now();
    std::set<std::string> keys(initial.begin(), initial.end());
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        keys.insert(key);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        sink += (keys.find(key) != keys.end() ? 1 : 0);
    }
    end = Clock::now();
    fill_lookup_stats(result, lookups.size(), elapsed_ms(start, end));
    result.memory_bytes = keys.size() * 96;
    (void)sink;
    return result;
}

BenchmarkResult bench_unordered_map(const std::vector<std::string>& initial,
                                    const std::vector<std::string>& inserts,
                                    const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "std::unordered_map point lookup";

    auto start = Clock::now();
    std::unordered_map<std::string, uint64_t> keys;
    keys.reserve(initial.size() + inserts.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        keys.emplace(initial[i], i);
    }
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        keys.emplace(inserts[i], initial.size() + i);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile uint64_t sink = 0;
    start = Clock::now();
    for (const auto& key : lookups) {
        auto it = keys.find(key);
        if (it != keys.end()) {
            sink += it->second;
        }
    }
    end = Clock::now();
    fill_lookup_stats(result, lookups.size(), elapsed_ms(start, end));
    result.memory_bytes = keys.size() * 112;
    (void)sink;
    return result;
}

std::vector<BenchmarkResult> run_dataset(const std::string& dataset) {
    auto initial = read_keys("data/" + dataset + "_initial.txt");
    auto inserts = read_keys("data/" + dataset + "_insert.txt");
    if (initial.empty() || inserts.empty()) {
        return {};
    }

    std::vector<std::string> lookups = initial;
    lookups.insert(lookups.end(), inserts.begin(), inserts.end());
    lookups = sorted_unique(std::move(lookups));
    std::mt19937 rng(2026);
    std::shuffle(lookups.begin(), lookups.end(), rng);

    std::vector<BenchmarkResult> results;
    results.push_back(bench_hrtli(initial, inserts, lookups));
    results.push_back(bench_hrtli_v3(initial, inserts, lookups));
    if (initial.size() + inserts.size() < 1000000) {
        results.push_back(bench_sorted_vector(initial, inserts, lookups));
        results.push_back(bench_std_set(initial, inserts, lookups));
    } else {
        std::cout << "  (Skipping sorted vector and std::set on large dataset to avoid hang/OOM)\n";
    }
    results.push_back(bench_unordered_map(initial, inserts, lookups));

    std::cout << "\nDataset: " << dataset << " (" << initial.size()
              << " initial, " << inserts.size() << " inserts)\n";
    std::cout << std::left << std::setw(34) << "Index"
              << std::right << std::setw(12) << "Load ms"
              << std::setw(12) << "Insert ms"
              << std::setw(14) << "Lookup Mops"
              << std::setw(14) << "Latency ns"
              << std::setw(12) << "Memory KB" << "\n";
    std::cout << std::string(98, '-') << "\n";
    for (const auto& row : results) {
        std::cout << std::left << std::setw(34) << row.index_name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(3) << row.load_time_ms
                  << std::setw(12) << row.insert_time_ms
                  << std::setw(14) << row.lookup_throughput_mops
                  << std::setw(14) << row.avg_lookup_latency_ns
                  << std::setw(12) << (row.memory_bytes / 1024.0) << "\n";
    }
    return results;
}

void write_json(const std::string& path,
                const std::vector<std::pair<std::string, std::vector<BenchmarkResult>>>& all_results) {
    std::ofstream file(path);
    file << "{\n";
    for (size_t i = 0; i < all_results.size(); ++i) {
        const auto& dataset = all_results[i].first;
        const auto& rows = all_results[i].second;
        file << "  \"" << dataset << "\": [\n";
        for (size_t j = 0; j < rows.size(); ++j) {
            const auto& row = rows[j];
            file << "    {"
                 << "\"index_name\": \"" << row.index_name << "\", "
                 << "\"load_time_ms\": " << row.load_time_ms << ", "
                 << "\"insert_time_ms\": " << row.insert_time_ms << ", "
                 << "\"lookup_throughput_mops\": " << row.lookup_throughput_mops << ", "
                 << "\"avg_lookup_latency_ns\": " << row.avg_lookup_latency_ns << ", "
                 << "\"memory_bytes\": " << row.memory_bytes << "}";
            file << (j + 1 == rows.size() ? "\n" : ",\n");
        }
        file << "  ]" << (i + 1 == all_results.size() ? "\n" : ",\n");
    }
    file << "}\n";
}

int main() {
    const std::vector<std::string> datasets = {
        "synthetic", "filesystem", "url", "dns", "json", "package", "packages", "wiki_ts", "osm_cellids"
    };
    std::vector<std::pair<std::string, std::vector<BenchmarkResult>>> all_results;
    for (const auto& dataset : datasets) {
        auto rows = run_dataset(dataset);
        if (!rows.empty()) {
            all_results.emplace_back(dataset, std::move(rows));
        }
    }
    write_json("results_q1/cpp_benchmark_results.json", all_results);
    std::cout << "\nSaved native benchmark results to results_q1/cpp_benchmark_results.json\n";
    std::cout << "External baseline binding macros available:";
    for (const auto& spec : hrtli::external::binding_specs()) {
        std::cout << " " << spec.macro;
    }
    std::cout << "\n";
    return 0;
}
