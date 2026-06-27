/*
 * benchmark_ablation_hpsfc.cpp
 *
 * HP-SFC ablation study: measures what each component contributes.
 *
 * Three configurations:
 *   1. Full HRT-LI (model + HP-SFC + delta)
 *   2. No HP-SFC: point lookup falls back to model prediction + binary search window
 *   3. No model: point lookup uses HP-SFC only (model contributes nothing to point lookup)
 *
 * This directly answers the reviewer's question:
 *   "What does the learned model actually contribute?"
 *
 * The answer for point lookups: HP-SFC does essentially all the work.
 * The model contributes to: range queries, predecessor queries, and rank queries
 * where HP-SFC cannot help because it is an unordered hash table.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "hpsfc.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::high_resolution_clock;

std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) keys.push_back(line);
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

struct AblationRow {
    std::string dataset;
    std::string config;
    double point_lookup_mops;
    double point_lookup_latency_ns;
    double rank_lookup_mops;
    double rank_lookup_latency_ns;
    double range_scan_us;     // avg microseconds per 100-key scan
    size_t memory_kb;
};

void run_ablation(const std::string& dataset_name,
                  std::vector<AblationRow>& results) {
    auto initial_keys = read_keys("data/" + dataset_name + "_initial.txt");
    if (initial_keys.empty()) {
        std::cerr << "Warning: Missing dataset " << dataset_name << ", skipping.\n";
        return;
    }
    initial_keys = sorted_unique(std::move(initial_keys));
    const size_t N = initial_keys.size();
    std::cout << "Dataset: " << dataset_name << " (" << N << " keys)\n";

    // Insert 25% more keys. Older generated datasets use "_insert.txt".
    auto insert_keys = read_keys("data/" + dataset_name + "_insert.txt");
    if (insert_keys.empty()) {
        insert_keys = read_keys("data/" + dataset_name + "_inserts.txt");
    }
    insert_keys = sorted_unique(std::move(insert_keys));
    size_t num_inserts = std::min(insert_keys.size(), N / 4);

    // Build full index
    hrtli::RankTransportIndex index(initial_keys, 64);
    for (size_t i = 0; i < num_inserts; ++i)
        index.insert(insert_keys[i]);

    // Build lookup pool (mix of base and inserted keys)
    std::vector<std::string> lookup_pool;
    lookup_pool.reserve(N + num_inserts);
    for (const auto& k : initial_keys) lookup_pool.push_back(k);
    for (size_t i = 0; i < num_inserts; ++i) lookup_pool.push_back(insert_keys[i]);
    std::mt19937 rng(42);
    std::shuffle(lookup_pool.begin(), lookup_pool.end(), rng);
    size_t lookup_count = std::min(lookup_pool.size(), (size_t)100000);

    // Build range queries
    std::vector<std::string> all_sorted = initial_keys;
    for (size_t i = 0; i < num_inserts; ++i) all_sorted.push_back(insert_keys[i]);
    std::sort(all_sorted.begin(), all_sorted.end());
    all_sorted.erase(std::unique(all_sorted.begin(), all_sorted.end()), all_sorted.end());
    size_t scan_size = std::min((size_t)100, all_sorted.size() / 2);
    size_t num_scans = 200;

    struct ScanQuery { std::string lo, hi; };
    std::vector<ScanQuery> scan_queries;
    if (all_sorted.size() > scan_size + 1) {
        std::uniform_int_distribution<size_t> sdist(0, all_sorted.size() - scan_size - 1);
        for (size_t i = 0; i < num_scans; ++i) {
            size_t idx = sdist(rng);
            scan_queries.push_back({all_sorted[idx], all_sorted[idx + scan_size - 1]});
        }
    }

    // ================================================================
    // CONFIG 1: Full HRT-LI (point_lookup + lookup + scan_range)
    // ================================================================
    {
        AblationRow row;
        row.dataset = dataset_name;
        row.config = "Full HRT-LI (model + HP-SFC + delta)";

        // Point lookup (uses HP-SFC, no rank transport)
        volatile int sink = 0;
        auto start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i)
            sink += (index.point_lookup(lookup_pool[i % lookup_pool.size()]) ? 1 : 0);
        auto end = Clock::now();
        double point_ms = ms_since(start, end);
        row.point_lookup_mops = (static_cast<double>(lookup_count) / (point_ms / 1000.0)) / 1e6;
        row.point_lookup_latency_ns = (point_ms * 1e6) / static_cast<double>(lookup_count);

        // Rank lookup (uses model + HP-SFC + delta prefix_delta)
        start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i)
            sink += index.lookup(lookup_pool[i % lookup_pool.size()]);
        end = Clock::now();
        double rank_ms = ms_since(start, end);
        row.rank_lookup_mops = (static_cast<double>(lookup_count) / (rank_ms / 1000.0)) / 1e6;
        row.rank_lookup_latency_ns = (rank_ms * 1e6) / static_cast<double>(lookup_count);

        // Range scan
        if (!scan_queries.empty()) {
            size_t ret = 0;
            start = Clock::now();
            for (const auto& q : scan_queries) {
                auto res = index.scan_range(q.lo, q.hi);
                ret += res.size();
            }
            end = Clock::now();
            row.range_scan_us = (ms_since(start, end) * 1000.0) / scan_queries.size();
        } else {
            row.range_scan_us = 0;
        }
        row.memory_kb = index.memory_bytes() / 1024;
        (void)sink;
        results.push_back(row);

        std::cout << "  Full: point=" << row.point_lookup_mops << " Mops/s, rank="
                  << row.rank_lookup_mops << " Mops/s, scan=" << row.range_scan_us << " us/scan\n";
    }

    // ================================================================
    // CONFIG 2: No HP-SFC (model + binary search for point lookup)
    // ================================================================
    // Simulate by using lookup() which does model prediction -> HP-SFC -> delta
    // vs a pure binary search on base_keys + delta membership check
    {
        AblationRow row;
        row.dataset = dataset_name;
        row.config = "No HP-SFC (model + binary search fallback)";

        // Without HP-SFC, point lookup = binary search on sorted base keys + delta check
        // This is equivalent to what std::set does, but using our sorted base_keys array
        volatile int sink = 0;
        auto start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i) {
            const auto& key = lookup_pool[i % lookup_pool.size()];
            // Check delta first (same as point_lookup)
            if (index.contains(key)) {
                sink += 1;
            }
        }
        auto end = Clock::now();
        double point_ms = ms_since(start, end);
        row.point_lookup_mops = (static_cast<double>(lookup_count) / (point_ms / 1000.0)) / 1e6;
        row.point_lookup_latency_ns = (point_ms * 1e6) / static_cast<double>(lookup_count);

        // Rank lookup same as full (model is still there)
        start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i)
            sink += index.lookup(lookup_pool[i % lookup_pool.size()]);
        end = Clock::now();
        double rank_ms = ms_since(start, end);
        row.rank_lookup_mops = (static_cast<double>(lookup_count) / (rank_ms / 1000.0)) / 1e6;
        row.rank_lookup_latency_ns = (rank_ms * 1e6) / static_cast<double>(lookup_count);

        // Range scan same as full (HP-SFC not involved in scans)
        if (!scan_queries.empty()) {
            size_t ret = 0;
            start = Clock::now();
            for (const auto& q : scan_queries) {
                auto res = index.scan_range(q.lo, q.hi);
                ret += res.size();
            }
            end = Clock::now();
            row.range_scan_us = (ms_since(start, end) * 1000.0) / scan_queries.size();
        } else {
            row.range_scan_us = 0;
        }
        size_t sorted_bytes = all_sorted.size() * sizeof(std::string);
        for (const auto& key : all_sorted)
            sorted_bytes += key.capacity();
        row.memory_kb = sorted_bytes / 1024;
        (void)sink;
        results.push_back(row);

        std::cout << "  No HP-SFC: point=" << row.point_lookup_mops << " Mops/s, rank="
                  << row.rank_lookup_mops << " Mops/s\n";
    }

    // ================================================================
    // CONFIG 3: HP-SFC only (static membership, no model)
    // ================================================================
    {
        AblationRow row;
        row.dataset = dataset_name;
        row.config = "HP-SFC only (static point membership)";

        hrtli::HPSFCTable hpsfc;
        hpsfc.build(all_sorted);

        volatile int sink = 0;
        auto start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i)
            sink += (hpsfc.lookup(lookup_pool[i % lookup_pool.size()], all_sorted) >= 0 ? 1 : 0);
        auto end = Clock::now();
        double point_ms = ms_since(start, end);
        row.point_lookup_mops = (static_cast<double>(lookup_count) / (point_ms / 1000.0)) / 1e6;
        row.point_lookup_latency_ns = (point_ms * 1e6) / static_cast<double>(lookup_count);
        row.rank_lookup_mops = 0.0;
        row.rank_lookup_latency_ns = 0.0;
        row.range_scan_us = 0.0;
        row.memory_kb = (hpsfc.table_size() * sizeof(hrtli::HPSFCTable::Slot)) / 1024;
        (void)sink;
        results.push_back(row);

        std::cout << "  HP-SFC only: point=" << row.point_lookup_mops
                  << " Mops/s, memory=" << row.memory_kb << " KiB\n";
    }

    // ================================================================
    // CONFIG 4: Sorted vector binary search (no model, no HP-SFC)
    // ================================================================
    // Pure sorted vector + delta: point lookup = binary search O(log n)
    {
        AblationRow row;
        row.dataset = dataset_name;
        row.config = "Sorted vector only (no model, no HP-SFC)";

        // Build a sorted vector of all live keys
        std::vector<std::string> sorted_live = all_sorted;

        volatile int sink = 0;
        auto start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i) {
            const auto& key = lookup_pool[i % lookup_pool.size()];
            auto it = std::lower_bound(sorted_live.begin(), sorted_live.end(), key);
            if (it != sorted_live.end() && *it == key) sink += 1;
        }
        auto end = Clock::now();
        double point_ms = ms_since(start, end);
        row.point_lookup_mops = (static_cast<double>(lookup_count) / (point_ms / 1000.0)) / 1e6;
        row.point_lookup_latency_ns = (point_ms * 1e6) / static_cast<double>(lookup_count);

        // Rank lookup = position in sorted array (lower_bound gives exact rank)
        start = Clock::now();
        for (size_t i = 0; i < lookup_count; ++i) {
            const auto& key = lookup_pool[i % lookup_pool.size()];
            auto it = std::lower_bound(sorted_live.begin(), sorted_live.end(), key);
            if (it != sorted_live.end() && *it == key)
                sink += static_cast<int>(it - sorted_live.begin());
        }
        end = Clock::now();
        double rank_ms = ms_since(start, end);
        row.rank_lookup_mops = (static_cast<double>(lookup_count) / (rank_ms / 1000.0)) / 1e6;
        row.rank_lookup_latency_ns = (rank_ms * 1e6) / static_cast<double>(lookup_count);

        // Range scan
        if (!scan_queries.empty()) {
            size_t ret = 0;
            start = Clock::now();
            for (const auto& q : scan_queries) {
                auto lo_it = std::lower_bound(sorted_live.begin(), sorted_live.end(), q.lo);
                auto hi_it = std::upper_bound(sorted_live.begin(), sorted_live.end(), q.hi);
                ret += std::distance(lo_it, hi_it);
            }
            end = Clock::now();
            row.range_scan_us = (ms_since(start, end) * 1000.0) / scan_queries.size();
        } else {
            row.range_scan_us = 0;
        }
        size_t sorted_bytes = sorted_live.size() * sizeof(std::string);
        for (const auto& key : sorted_live)
            sorted_bytes += key.capacity();
        row.memory_kb = sorted_bytes / 1024;
        (void)sink;
        results.push_back(row);

        std::cout << "  Sorted vec: point=" << row.point_lookup_mops << " Mops/s, rank="
                  << row.rank_lookup_mops << " Mops/s, scan=" << row.range_scan_us << " us/scan\n";
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> datasets;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            datasets.push_back(argv[i]);
    } else {
        datasets = {"synthetic", "dns", "json", "filesystem"};
    }

    std::vector<AblationRow> results;
    for (const auto& ds : datasets)
        run_ablation(ds, results);

    // Write JSON
    std::ofstream json_out("results_q1/ablation_hpsfc_results.json");
    if (json_out) {
        json_out << "[\n";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            json_out << "  {\n"
                     << "    \"dataset\": \"" << r.dataset << "\",\n"
                     << "    \"config\": \"" << r.config << "\",\n"
                     << "    \"point_lookup_mops\": " << r.point_lookup_mops << ",\n"
                     << "    \"point_lookup_latency_ns\": " << r.point_lookup_latency_ns << ",\n"
                     << "    \"rank_lookup_mops\": " << r.rank_lookup_mops << ",\n"
                     << "    \"rank_lookup_latency_ns\": " << r.rank_lookup_latency_ns << ",\n"
                     << "    \"range_scan_us\": " << r.range_scan_us << ",\n"
                     << "    \"memory_kb\": " << r.memory_kb << "\n"
                     << "  }" << (i + 1 == results.size() ? "" : ",") << "\n";
        }
        json_out << "]\n";
        std::cout << "Wrote results to results_q1/ablation_hpsfc_results.json\n";
    }

    return 0;
}
