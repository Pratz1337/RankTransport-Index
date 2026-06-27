#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::high_resolution_clock;

struct LatencyStats {
    double p50_ns = 0.0;
    double p95_ns = 0.0;
    double p99_ns = 0.0;
    double avg_ns = 0.0;
};

struct RangeStats {
    size_t target_result_size = 0;
    double avg_result_size = 0.0;
    LatencyStats count_range;
    LatencyStats scan_range;
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

double percentile(std::vector<long long> values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    double pos = (static_cast<double>(values.size() - 1)) * q;
    size_t lo = static_cast<size_t>(pos);
    size_t hi = std::min(lo + 1, values.size() - 1);
    double frac = pos - static_cast<double>(lo);
    return static_cast<double>(values[lo]) * (1.0 - frac) +
           static_cast<double>(values[hi]) * frac;
}

LatencyStats summarize(const std::vector<long long>& latencies) {
    LatencyStats stats;
    stats.p50_ns = percentile(latencies, 0.50);
    stats.p95_ns = percentile(latencies, 0.95);
    stats.p99_ns = percentile(latencies, 0.99);
    long double total = 0.0;
    for (long long value : latencies) {
        total += static_cast<long double>(value);
    }
    stats.avg_ns = latencies.empty() ? 0.0 : static_cast<double>(total / latencies.size());
    return stats;
}

std::vector<std::pair<std::string, std::string>> make_ranges(
    const std::vector<std::string>& snapshot,
    size_t target_size,
    size_t n_ranges,
    uint32_t seed) {
    std::vector<std::pair<std::string, std::string>> ranges;
    if (snapshot.empty()) {
        return ranges;
    }
    std::mt19937 rng(seed);
    size_t max_start = snapshot.size() > target_size ? snapshot.size() - target_size : 0;
    std::uniform_int_distribution<size_t> dist(0, max_start);
    ranges.reserve(n_ranges);
    for (size_t i = 0; i < n_ranges; ++i) {
        size_t start = dist(rng);
        size_t end = std::min(snapshot.size() - 1, start + target_size - 1);
        ranges.emplace_back(snapshot[start], snapshot[end]);
    }
    return ranges;
}

template <typename IndexType>
RangeStats benchmark_selectivity(
    const IndexType& index,
    const std::vector<std::string>& snapshot,
    size_t target_size,
    size_t n_ranges,
    uint32_t seed) {
    auto ranges = make_ranges(snapshot, target_size, n_ranges, seed);
    std::vector<long long> count_latencies;
    std::vector<long long> scan_latencies;
    count_latencies.reserve(ranges.size());
    scan_latencies.reserve(ranges.size());

    size_t total_results = 0;
    volatile size_t sink = 0;
    for (const auto& range : ranges) {
        auto start = Clock::now();
        int count = index.count_range(range.first, range.second);
        auto end = Clock::now();
        count_latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

        start = Clock::now();
        auto scanned = index.scan_range(range.first, range.second);
        end = Clock::now();
        scan_latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());

        if (count != static_cast<int>(scanned.size())) {
            std::cerr << "range mismatch for [" << range.first << ", " << range.second
                      << "]: count=" << count << " scan=" << scanned.size() << "\n";
            std::abort();
        }
        total_results += scanned.size();
        sink += scanned.size();
    }
    (void)sink;

    RangeStats stats;
    stats.target_result_size = target_size;
    stats.avg_result_size = ranges.empty() ? 0.0 : static_cast<double>(total_results) / ranges.size();
    stats.count_range = summarize(count_latencies);
    stats.scan_range = summarize(scan_latencies);
    return stats;
}

void write_json(const std::string& path,
                const std::string& dataset,
                size_t base_keys,
                size_t inserts,
                size_t deletes,
                size_t live_keys,
                size_t mutation_count,
                size_t ranges_per_selectivity,
                const std::vector<RangeStats>& stats_v2,
                const std::vector<RangeStats>& stats_v3) {
    std::ofstream file(path);
    file << "{\n";
    file << "  \"dataset\": \"" << dataset << "\",\n";
    file << "  \"config\": {\n";
    file << "    \"base_keys\": " << base_keys << ",\n";
    file << "    \"inserts\": " << inserts << ",\n";
    file << "    \"deletes\": " << deletes << ",\n";
    file << "    \"live_keys\": " << live_keys << ",\n";
    file << "    \"mutation_count\": " << mutation_count << ",\n";
    file << "    \"ranges_per_selectivity\": " << ranges_per_selectivity << "\n";
    file << "  },\n";
    
    file << "  \"selectivities\": {\n";
    for (size_t i = 0; i < stats_v2.size(); ++i) {
        const auto& row = stats_v2[i];
        file << "    \"" << row.target_result_size << "\": {\n";
        file << "      \"target_result_size\": " << row.target_result_size << ",\n";
        file << "      \"avg_result_size\": " << row.avg_result_size << ",\n";
        file << "      \"count_range\": {\"p50_ns\": " << row.count_range.p50_ns
             << ", \"p95_ns\": " << row.count_range.p95_ns
             << ", \"p99_ns\": " << row.count_range.p99_ns
             << ", \"avg_ns\": " << row.count_range.avg_ns << "},\n";
        file << "      \"scan_range\": {\"p50_ns\": " << row.scan_range.p50_ns
             << ", \"p95_ns\": " << row.scan_range.p95_ns
             << ", \"p99_ns\": " << row.scan_range.p99_ns
             << ", \"avg_ns\": " << row.scan_range.avg_ns << "}\n";
        file << "    }" << (i + 1 == stats_v2.size() ? "\n" : ",\n");
    }
    file << "  },\n";

    file << "  \"v3\": {\n";
    file << "    \"selectivities\": {\n";
    for (size_t i = 0; i < stats_v3.size(); ++i) {
        const auto& row = stats_v3[i];
        file << "      \"" << row.target_result_size << "\": {\n";
        file << "        \"target_result_size\": " << row.target_result_size << ",\n";
        file << "        \"avg_result_size\": " << row.avg_result_size << ",\n";
        file << "        \"count_range\": {\"p50_ns\": " << row.count_range.p50_ns
             << ", \"p95_ns\": " << row.count_range.p95_ns
             << ", \"p99_ns\": " << row.count_range.p99_ns
             << ", \"avg_ns\": " << row.count_range.avg_ns << "},\n";
        file << "        \"scan_range\": {\"p50_ns\": " << row.scan_range.p50_ns
             << ", \"p95_ns\": " << row.scan_range.p95_ns
             << ", \"p99_ns\": " << row.scan_range.p99_ns
             << ", \"avg_ns\": " << row.scan_range.avg_ns << "}\n";
        file << "      }" << (i + 1 == stats_v3.size() ? "\n" : ",\n");
    }
    file << "    }\n";
    file << "  }\n";
    file << "}\n";
}

int main(int argc, char** argv) {
    std::string dataset = argc > 1 ? argv[1] : "synthetic";
    auto initial = read_keys("data/" + dataset + "_initial.txt");
    auto inserts = read_keys("data/" + dataset + "_insert.txt");
    if (initial.empty() || inserts.empty()) {
        std::cerr << "missing dataset files for " << dataset << "\n";
        return 1;
    }

    hrtli::RankTransportIndex index(initial, 64);
    hrtli::RankTransportIndexV3 index_v3(initial, 64);
    for (const auto& key : inserts) {
        index.insert(key);
        index_v3.insert(key);
    }

    std::vector<std::string> deletion_candidates = initial;
    std::mt19937 rng(2026);
    std::shuffle(deletion_candidates.begin(), deletion_candidates.end(), rng);
    size_t deletes = std::min<size_t>(1000, deletion_candidates.size() / 8);
    for (size_t i = 0; i < deletes; ++i) {
        index.remove(deletion_candidates[i]);
        index_v3.remove(deletion_candidates[i]);
    }

    auto snapshot = index.snapshot_keys();
    auto snapshot_v3 = index_v3.snapshot_keys();
    const size_t ranges_per_selectivity = 200;
    std::vector<size_t> selectivities = {1, 10, 100, 1000};
    std::vector<RangeStats> rows;
    rows.reserve(selectivities.size());
    std::vector<RangeStats> rows_v3;
    rows_v3.reserve(selectivities.size());

    std::cout << "Native range benchmark: " << dataset
              << " live_keys=" << snapshot.size()
              << " mutation_count=" << index.mutation_count() << "\n";
    for (size_t target : selectivities) {
        rows.push_back(benchmark_selectivity(
            index, snapshot, target, ranges_per_selectivity, static_cast<uint32_t>(2026 + target)));
        const auto& row = rows.back();
        std::cout << "  v2 k~" << target
                  << " count p99=" << std::fixed << std::setprecision(1)
                  << (row.count_range.p99_ns / 1000.0) << " us"
                  << " scan p99=" << (row.scan_range.p99_ns / 1000.0) << " us\n";

        rows_v3.push_back(benchmark_selectivity(
            index_v3, snapshot_v3, target, ranges_per_selectivity, static_cast<uint32_t>(2026 + target)));
        const auto& row_v3 = rows_v3.back();
        std::cout << "  v3 k~" << target
                  << " count p99=" << std::fixed << std::setprecision(1)
                  << (row_v3.count_range.p99_ns / 1000.0) << " us"
                  << " scan p99=" << (row_v3.scan_range.p99_ns / 1000.0) << " us\n";
    }

    std::string out = "results_q1/cpp_range_latency_" + dataset + ".json";
    write_json(out, dataset, initial.size(), inserts.size(), deletes, snapshot.size(),
               index.mutation_count(), ranges_per_selectivity, rows, rows_v3);
    std::cout << "Saved native range benchmark to " << out << "\n";
    return 0;
}
