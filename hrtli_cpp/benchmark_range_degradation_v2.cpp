/*
 * benchmark_range_degradation_v2.cpp
 *
 * Fair delete-heavy range scan comparison:
 *   1. Dynamic PGM (no compaction — tombstones accumulate)
 *   2. Dynamic PGM (with compaction — rebuilt from live keys each step)
 *   3. std::set (ordered tree proxy — represents ART/HOT/B+-tree class;
 *      physically removes deleted keys, no tombstone bloat)
 *   4. HRT-LI (no consolidation)
 *   5. HRT-LI (with consolidation)
 *
 * Fixed-output methodology: at each deletion ratio, range bounds are chosen
 * from live keys so each scan returns exactly target_live_scan_size keys.
 * This removes the confound where high deletion ratios produce smaller result
 * sets.
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
#include <set>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::high_resolution_clock;

struct RangeQuery {
    std::string lo_key;
    std::string hi_key;
    double lo_surrogate;
    double hi_surrogate;
};

struct DegradationRow {
    std::string dataset;
    double deleted_ratio;
    std::string index_name;
    double total_scan_time_ms;
    double avg_scan_time_us;
    size_t total_returned;
    double consolidation_time_ms = 0.0;
};

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

std::unordered_map<std::string, double> build_surrogate_keys(
    const std::vector<std::string>& all_keys) {
    std::unordered_map<std::string, double> out;
    out.reserve(all_keys.size());
    for (size_t i = 0; i < all_keys.size(); ++i)
        out.emplace(all_keys[i], static_cast<double>(i + 1));
    return out;
}

void run_degradation_for_dataset(const std::string& dataset_name,
                                 std::vector<DegradationRow>& all_results) {
    auto initial_keys = read_keys("data/" + dataset_name + "_initial.txt");
    if (initial_keys.empty()) {
        std::cerr << "Warning: Missing dataset " << dataset_name << ", skipping.\n";
        return;
    }
    initial_keys = sorted_unique(std::move(initial_keys));
    const size_t N = initial_keys.size();
    std::cout << "Loaded dataset: " << dataset_name << " with " << N << " unique sorted keys.\n";

    if (N < 200) {
        std::cerr << "Dataset too small for range degradation benchmark.\n";
        return;
    }

    auto surrogate = build_surrogate_keys(initial_keys);

    const size_t target_live_scan_size = 100;
    const size_t num_queries = 200;
    std::mt19937 rng(2026);

    std::vector<double> deletion_ratios = {0.0, 0.2, 0.5, 0.8, 0.95};

    // ---- Build all index instances ----

    // 1. Dynamic PGM (no compaction)
    std::vector<std::pair<double, uint64_t>> bulk;
    bulk.reserve(N);
    for (size_t i = 0; i < N; ++i)
        bulk.emplace_back(surrogate.at(initial_keys[i]), i + 1);
    std::sort(bulk.begin(), bulk.end());
    hrtli::external::DynamicPgmIndex<double, uint64_t> pgm_nocompact(bulk.begin(), bulk.end());

    // 2. std::set (ordered tree proxy for ART/HOT)
    std::set<std::string> ordered_tree(initial_keys.begin(), initial_keys.end());

    // 3. HRT-LI (no consolidation)
    hrtli::RankTransportIndex hrtli_nocons(initial_keys, 64);

    // 4. HRT-LI (with consolidation)
    hrtli::RankTransportIndex hrtli_cons(initial_keys, 64);

    // Shuffle deletion order
    std::vector<std::string> keys_to_delete = initial_keys;
    std::shuffle(keys_to_delete.begin(), keys_to_delete.end(), rng);

    size_t last_deleted_count = 0;

    for (double d : deletion_ratios) {
        size_t target_deleted_count = static_cast<size_t>(std::round(N * d));

        // Perform deletions across all structures
        for (size_t i = last_deleted_count; i < target_deleted_count; ++i) {
            const auto& key = keys_to_delete[i];
            pgm_nocompact.erase(surrogate.at(key));
            ordered_tree.erase(key);
            hrtli_nocons.remove(key);
            hrtli_cons.remove(key);
        }

        // HRT-LI consolidation
        double consolidation_time_ms = 0.0;
        if (d > 0.0) {
            auto cons_start = Clock::now();
            hrtli_cons.consolidate();
            hrtli_cons.wait_rebuild();
            auto cons_end = Clock::now();
            consolidation_time_ms = ms_since(cons_start, cons_end);
        }

        last_deleted_count = target_deleted_count;

        std::cout << "  Deletion ratio: " << (d * 100) << "% (deleted: "
                  << target_deleted_count << "/" << N << ")\n";

        // Build live-key list for fixed-output queries
        std::unordered_set<std::string> deleted_keys;
        deleted_keys.reserve(target_deleted_count * 2 + 1);
        for (size_t i = 0; i < target_deleted_count; ++i)
            deleted_keys.insert(keys_to_delete[i]);
        std::vector<std::string> live_keys;
        live_keys.reserve(N - target_deleted_count);
        for (const auto& key : initial_keys) {
            if (deleted_keys.find(key) == deleted_keys.end())
                live_keys.push_back(key);
        }
        if (live_keys.size() <= target_live_scan_size) {
            std::cerr << "Not enough live keys at deletion ratio " << d << "\n";
            continue;
        }

        // Build fixed-output range queries from live keys
        std::vector<RangeQuery> queries;
        queries.reserve(num_queries);
        std::uniform_int_distribution<size_t> dist(0, live_keys.size() - target_live_scan_size - 1);
        for (size_t i = 0; i < num_queries; ++i) {
            size_t idx = dist(rng);
            const std::string& lo = live_keys[idx];
            const std::string& hi = live_keys[idx + target_live_scan_size - 1];
            queries.push_back({lo, hi, surrogate.at(lo), surrogate.at(hi)});
        }

        // ---- Benchmark PGM (no compaction) ----
        {
            size_t returned_keys = 0;
            auto start = Clock::now();
            for (const auto& q : queries) {
                auto it = pgm_nocompact.lower_bound(q.lo_surrogate);
                while (it != pgm_nocompact.end() && it->first <= q.hi_surrogate) {
                    returned_keys++;
                    ++it;
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double avg_us = (time_ms * 1000.0) / num_queries;
            all_results.push_back({dataset_name, d, "Dynamic PGM (no compaction)", time_ms, avg_us, returned_keys, 0.0});
            std::cout << "    PGM (no compact): " << avg_us << " us/scan (ret " << returned_keys << ")\n";
        }

        // ---- Benchmark PGM (with compaction) ----
        // Rebuild PGM from live keys only — this is PGM's equivalent of consolidation.
        {
            std::vector<std::pair<double, uint64_t>> live_bulk;
            live_bulk.reserve(live_keys.size());
            for (size_t i = 0; i < live_keys.size(); ++i)
                live_bulk.emplace_back(surrogate.at(live_keys[i]), i + 1);
            std::sort(live_bulk.begin(), live_bulk.end());
            hrtli::external::DynamicPgmIndex<double, uint64_t> pgm_compacted(live_bulk.begin(), live_bulk.end());

            size_t returned_keys = 0;
            auto start = Clock::now();
            for (const auto& q : queries) {
                auto it = pgm_compacted.lower_bound(q.lo_surrogate);
                while (it != pgm_compacted.end() && it->first <= q.hi_surrogate) {
                    returned_keys++;
                    ++it;
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double avg_us = (time_ms * 1000.0) / num_queries;
            all_results.push_back({dataset_name, d, "Dynamic PGM (with compaction)", time_ms, avg_us, returned_keys, 0.0});
            std::cout << "    PGM (compacted): " << avg_us << " us/scan (ret " << returned_keys << ")\n";
        }

        // ---- Benchmark std::set (ordered tree — ART/HOT proxy) ----
        {
            size_t returned_keys = 0;
            auto start = Clock::now();
            for (const auto& q : queries) {
                auto it = ordered_tree.lower_bound(q.lo_key);
                while (it != ordered_tree.end() && *it <= q.hi_key) {
                    returned_keys++;
                    ++it;
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double avg_us = (time_ms * 1000.0) / num_queries;
            all_results.push_back({dataset_name, d, "std::set (ordered tree proxy)", time_ms, avg_us, returned_keys, 0.0});
            std::cout << "    std::set (tree): " << avg_us << " us/scan (ret " << returned_keys << ")\n";
        }

        // ---- Benchmark HRT-LI (no consolidation) ----
        {
            size_t returned_keys = 0;
            auto start = Clock::now();
            for (const auto& q : queries) {
                auto res = hrtli_nocons.scan_range(q.lo_key, q.hi_key);
                returned_keys += res.size();
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double avg_us = (time_ms * 1000.0) / num_queries;
            all_results.push_back({dataset_name, d, "HRT-LI (no consolidation)", time_ms, avg_us, returned_keys, 0.0});
            std::cout << "    HRT-LI (no cons): " << avg_us << " us/scan (ret " << returned_keys << ")\n";
        }

        // ---- Benchmark HRT-LI (with consolidation) ----
        {
            size_t returned_keys = 0;
            auto start = Clock::now();
            for (const auto& q : queries) {
                auto res = hrtli_cons.scan_range(q.lo_key, q.hi_key);
                returned_keys += res.size();
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double avg_us = (time_ms * 1000.0) / num_queries;
            all_results.push_back({dataset_name, d, "HRT-LI (with consolidation)", time_ms, avg_us, returned_keys, consolidation_time_ms});
            std::cout << "    HRT-LI (cons): " << avg_us << " us/scan (ret " << returned_keys
                      << ", consolidation " << consolidation_time_ms << " ms)\n";
        }
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> datasets;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            datasets.push_back(argv[i]);
    } else {
        datasets = {"synthetic", "dns", "filesystem"};
    }

    std::vector<DegradationRow> all_results;
    for (const auto& dataset : datasets)
        run_degradation_for_dataset(dataset, all_results);

    // Write CSV
    std::ofstream csv_out("results_q1/range_degradation_v2_results.csv");
    if (csv_out) {
        csv_out << "dataset,deleted_ratio,index_name,total_scan_time_ms,avg_scan_time_us,total_returned,consolidation_time_ms\n";
        for (const auto& row : all_results) {
            csv_out << row.dataset << "," << row.deleted_ratio << "," << row.index_name << ","
                    << row.total_scan_time_ms << "," << row.avg_scan_time_us << "," << row.total_returned
                    << "," << row.consolidation_time_ms << "\n";
        }
        std::cout << "Wrote results to results_q1/range_degradation_v2_results.csv\n";
    }

    // Write JSON
    std::ofstream json_out("results_q1/range_degradation_v2_results.json");
    if (json_out) {
        json_out << "[\n";
        for (size_t i = 0; i < all_results.size(); ++i) {
            const auto& row = all_results[i];
            json_out << "  {\n"
                     << "    \"dataset\": \"" << row.dataset << "\",\n"
                     << "    \"deleted_ratio\": " << row.deleted_ratio << ",\n"
                     << "    \"index_name\": \"" << row.index_name << "\",\n"
                     << "    \"total_scan_time_ms\": " << row.total_scan_time_ms << ",\n"
                     << "    \"avg_scan_time_us\": " << row.avg_scan_time_us << ",\n"
                     << "    \"total_returned\": " << row.total_returned << ",\n"
                     << "    \"consolidation_time_ms\": " << row.consolidation_time_ms << "\n"
                     << "  }" << (i + 1 == all_results.size() ? "" : ",") << "\n";
        }
        json_out << "]\n";
        std::cout << "Wrote results to results_q1/range_degradation_v2_results.json\n";
    }

    return 0;
}
