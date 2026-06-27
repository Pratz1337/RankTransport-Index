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
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::high_resolution_clock;

struct ResultRow {
    std::string dataset;
    double write_ratio;
    std::string index_name;
    double throughput_mops;
    double latency_ns;
    double time_ms;
    int trial;
};

#if defined(HRTLI_WITH_HOT)
struct StringPayload {
    const char* key;
    uint64_t value;
};

template <typename ValueType>
struct StringPayloadExtractor {
    using KeyType = const char*;
    KeyType operator()(ValueType const& payload) const { return payload->key; }
    KeyType operator()(KeyType key) const { return key; }
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

std::unordered_map<std::string, double> build_surrogate_keys(
    const std::vector<std::string>& all_keys) {
    std::unordered_map<std::string, double> out;
    out.reserve(all_keys.size());
    for (size_t i = 0; i < all_keys.size(); ++i) {
        out.emplace(all_keys[i], static_cast<double>(i + 1));
    }
    return out;
}

struct Op {
    bool is_write;
    std::string key;
    double surrogate_key;
};

const std::vector<double> WRITE_RATIOS = {0.0, 0.01, 0.02, 0.05, 0.10, 0.25, 0.50, 0.75, 1.0};

void run_sweep_for_dataset(const std::string& dataset_name,
                           int trial,
                           std::vector<ResultRow>& all_results) {
    auto initial_keys = read_keys("data/" + dataset_name + "_initial.txt");
    auto insert_keys = read_keys("data/" + dataset_name + "_insert.txt");
    if (initial_keys.empty() || insert_keys.empty()) {
        std::cerr << "Warning: Missing or empty dataset files for " << dataset_name << ", skipping.\n";
        return;
    }

    std::cout << "Loaded dataset: " << dataset_name << " (initial: " << initial_keys.size() 
              << ", insert: " << insert_keys.size() << ")" << std::endl;

    // Create unique set of all keys for surrogate mapping
    std::vector<std::string> all_keys = initial_keys;
    all_keys.insert(all_keys.end(), insert_keys.begin(), insert_keys.end());
    all_keys = sorted_unique(std::move(all_keys));
    auto surrogate = build_surrogate_keys(all_keys);

    const size_t workload_size = std::min<size_t>(100000, insert_keys.size());

    for (double w : WRITE_RATIOS) {
        int ratio_percent = static_cast<int>(std::round(w * 100.0));
        size_t num_writes = static_cast<size_t>(std::round(workload_size * w));
        size_t num_reads = workload_size - num_writes;

        std::cout << "  Write ratio: " << ratio_percent << "% (writes: " << num_writes 
                  << ", reads: " << num_reads << ")" << std::endl;

        // Generate operation sequence
        std::vector<Op> ops;
        ops.reserve(workload_size);

        // Add writes from insert_keys
        for (size_t i = 0; i < num_writes; ++i) {
            ops.push_back({true, insert_keys[i], surrogate.at(insert_keys[i])});
        }

        // Add reads from initial_keys (drawn with replacement to ensure we have enough)
        std::mt19937 rng(2026);
        std::uniform_int_distribution<size_t> dist(0, initial_keys.size() - 1);
        for (size_t i = 0; i < num_reads; ++i) {
            size_t rand_idx = dist(rng);
            ops.push_back({false, initial_keys[rand_idx], surrogate.at(initial_keys[rand_idx])});
        }

        // Shuffle operations to interleave them
        std::shuffle(ops.begin(), ops.end(), rng);

        // 1. HRT-LI
        {
            hrtli::RankTransportIndex index(initial_keys, 64);
            volatile int64_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    index.insert(op.key);
                } else {
                    sink += (index.point_lookup(op.key) ? 1 : 0);
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "HRT-LI", throughput_mops, latency_ns, time_ms, trial});
            (void)sink;
        }

        // 1b. HRT-LI v3
        {
            hrtli::RankTransportIndexV3 index(initial_keys, 64);
            volatile int64_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    index.insert(op.key);
                } else {
                    sink += (index.point_lookup(op.key) ? 1 : 0);
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "HRT-LI v3", throughput_mops, latency_ns, time_ms, trial});
            (void)sink;
        }

#if defined(HRTLI_WITH_ALEX)
        // 2. ALEX
        {
            std::vector<std::pair<double, uint64_t>> bulk;
            bulk.reserve(initial_keys.size());
            for (size_t i = 0; i < initial_keys.size(); ++i) {
                bulk.emplace_back(surrogate.at(initial_keys[i]), i + 1);
            }
            std::sort(bulk.begin(), bulk.end());
            hrtli::external::AlexIndex<double, uint64_t> index;
            index.bulk_load(bulk.data(), static_cast<int>(bulk.size()));

            volatile uint64_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    index.insert(op.surrogate_key, 12345);
                } else {
                    auto it = index.find(op.surrogate_key);
                    if (it != index.end()) {
                        sink += it.payload();
                    }
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "ALEX", throughput_mops, latency_ns, time_ms, trial});
            (void)sink;
        }
#endif

#if defined(HRTLI_WITH_PGM)
        // 3. Dynamic PGM-index
        {
            std::vector<std::pair<double, uint64_t>> bulk;
            bulk.reserve(initial_keys.size());
            for (size_t i = 0; i < initial_keys.size(); ++i) {
                bulk.emplace_back(surrogate.at(initial_keys[i]), i + 1);
            }
            std::sort(bulk.begin(), bulk.end());
            hrtli::external::DynamicPgmIndex<double, uint64_t> index(bulk.begin(), bulk.end());

            volatile uint64_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    index.insert_or_assign(op.surrogate_key, 12345);
                } else {
                    auto it = index.find(op.surrogate_key);
                    if (it != index.end()) {
                        sink += it->second;
                    }
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "Dynamic PGM", throughput_mops, latency_ns, time_ms, trial});
            (void)sink;
        }
#endif

#if defined(HRTLI_WITH_HOT)
        // 4. HOT
        {
            hot::singlethreaded::HOTSingleThreaded<StringPayload*, StringPayloadExtractor> index;
            std::vector<std::unique_ptr<StringPayload>> payloads;
            payloads.reserve(initial_keys.size() + num_writes);

            for (size_t i = 0; i < initial_keys.size(); ++i) {
                payloads.push_back(std::make_unique<StringPayload>(StringPayload{initial_keys[i].c_str(), i + 1}));
                index.insert(payloads.back().get());
            }

            std::vector<std::unique_ptr<StringPayload>> write_payloads;
            write_payloads.reserve(num_writes);
            for (const auto& op : ops) {
                if (op.is_write) {
                    write_payloads.push_back(std::make_unique<StringPayload>(StringPayload{op.key.c_str(), 12345}));
                }
            }

            size_t write_idx = 0;
            volatile uint64_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    index.insert(write_payloads[write_idx++].get());
                } else {
                    auto found = index.lookup(op.key.c_str());
                    if (found.mIsValid) {
                        sink += found.mValue->value;
                    }
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "HOT", throughput_mops, latency_ns, time_ms, trial});
            (void)sink;
        }
#endif

#if defined(HRTLI_WITH_ART)
        // 5. ART (libart)
        {
            art_tree index;
            art_tree_init(&index);

            for (size_t i = 0; i < initial_keys.size(); ++i) {
                art_insert(&index, reinterpret_cast<const unsigned char*>(initial_keys[i].c_str()),
                           static_cast<int>(initial_keys[i].size()), reinterpret_cast<void*>(i + 1));
            }

            volatile uintptr_t sink = 0;
            auto start = Clock::now();
            for (const auto& op : ops) {
                if (op.is_write) {
                    art_insert(&index, reinterpret_cast<const unsigned char*>(op.key.c_str()),
                               static_cast<int>(op.key.size()), reinterpret_cast<void*>(12345));
                } else {
                    sink += reinterpret_cast<uintptr_t>(
                        art_search(&index, reinterpret_cast<const unsigned char*>(op.key.c_str()),
                                   static_cast<int>(op.key.size())));
                }
            }
            auto end = Clock::now();
            double time_ms = ms_since(start, end);
            double throughput_mops = (static_cast<double>(ops.size()) / (time_ms / 1000.0)) / 1e6;
            double latency_ns = (time_ms * 1e6) / static_cast<double>(ops.size());
            all_results.push_back({dataset_name, w, "ART", throughput_mops, latency_ns, time_ms, trial});
            art_tree_destroy(&index);
            (void)sink;
        }
#endif
    }
}

int main(int argc, char** argv) {
    std::vector<std::string> datasets;
    int trials = 1;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--trials") {
                if (i + 1 >= argc) {
                    throw std::invalid_argument("--trials requires a value");
                }
                trials = std::max(1, std::stoi(argv[++i]));
            } else if (arg.rfind("--trials=", 0) == 0) {
                trials = std::max(1, std::stoi(arg.substr(9)));
            } else {
                datasets.push_back(arg);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 2;
    }

    if (datasets.empty()) {
        datasets = {"wiki_ts", "osm_cellids"};
    }

    std::vector<ResultRow> all_results;
    for (int trial = 1; trial <= trials; ++trial) {
        std::cout << "=== Trial " << trial << "/" << trials << " ===\n";
        for (const auto& dataset : datasets) {
            run_sweep_for_dataset(dataset, trial, all_results);
        }
    }

    // Write output to CSV
    std::ofstream csv_out("results_q1/write_sweep_results.csv");
    if (csv_out) {
        csv_out << "dataset,write_ratio,index_name,throughput_mops,latency_ns,time_ms,trial\n";
        for (const auto& row : all_results) {
            csv_out << row.dataset << "," << row.write_ratio << "," << row.index_name << ","
                    << row.throughput_mops << "," << row.latency_ns << "," << row.time_ms
                    << "," << row.trial << "\n";
        }
        std::cout << "Wrote results to results_q1/write_sweep_results.csv" << std::endl;
    }

    // Write output to JSON
    std::ofstream json_out("results_q1/write_sweep_results.json");
    if (json_out) {
        json_out << "[\n";
        for (size_t i = 0; i < all_results.size(); ++i) {
            const auto& row = all_results[i];
            json_out << "  {\n"
                     << "    \"dataset\": \"" << row.dataset << "\",\n"
                     << "    \"write_ratio\": " << row.write_ratio << ",\n"
                     << "    \"index_name\": \"" << row.index_name << "\",\n"
                     << "    \"throughput_mops\": " << row.throughput_mops << ",\n"
                     << "    \"latency_ns\": " << row.latency_ns << ",\n"
                     << "    \"time_ms\": " << row.time_ms << ",\n"
                     << "    \"trial\": " << row.trial << "\n"
                     << "  }" << (i + 1 == all_results.size() ? "" : ",") << "\n";
        }
        json_out << "]\n";
        std::cout << "Wrote results to results_q1/write_sweep_results.json" << std::endl;
    }

    // Identify crossover points
    std::cout << "\n=== CROSSOVER ANALYSIS ===\n";
    // For each dataset and competitor, find at what write ratio HRT-LI becomes faster or slower than the competitor.
    // Group results by dataset, write_ratio, index_name.
    std::unordered_map<std::string, std::unordered_map<double, std::unordered_map<std::string, std::pair<double, int>>>> accum_map;
    for (const auto& row : all_results) {
        auto& bucket = accum_map[row.dataset][row.write_ratio][row.index_name];
        bucket.first += row.throughput_mops;
        bucket.second += 1;
    }
    std::unordered_map<std::string, std::unordered_map<double, std::unordered_map<std::string, double>>> lookup_map;
    for (const auto& dataset_entry : accum_map) {
        for (const auto& ratio_entry : dataset_entry.second) {
            for (const auto& index_entry : ratio_entry.second) {
                lookup_map[dataset_entry.first][ratio_entry.first][index_entry.first] =
                    index_entry.second.first / static_cast<double>(index_entry.second.second);
            }
        }
    }

    for (const auto& dataset : datasets) {
        std::cout << "Dataset: " << dataset << "\n";
        std::vector<std::string> competitors = {"ALEX", "Dynamic PGM", "HOT", "ART"};
        for (const auto& comp : competitors) {
            bool has_comp = false;
            // Check if competitor is present in results
            for (double w : WRITE_RATIOS) {
                if (lookup_map[dataset][w].count(comp)) {
                    has_comp = true;
                    break;
                }
            }
            if (!has_comp) continue;

            std::cout << "  vs " << comp << ":\n";
            int crossover_ratio = -1;
            std::string state = "";
            double previous_w = -1.0;
            for (double w : WRITE_RATIOS) {
                int ratio = static_cast<int>(std::round(w * 100.0));
                double hrtli_val = lookup_map[dataset][w]["HRT-LI"];
                double comp_val = lookup_map[dataset][w][comp];
                std::string current_state = (hrtli_val > comp_val) ? "HRT-LI faster" : "HRT-LI slower";
                if (ratio == 0) {
                    state = current_state;
                    std::cout << "    At 0% write: HRT-LI is " << (hrtli_val > comp_val ? "faster" : "slower") 
                              << " (HRT-LI: " << hrtli_val << " Mops/s vs " << comp << ": " << comp_val << " Mops/s)\n";
                } else if (current_state != state) {
                    std::cout << "    [Crossover] Between " << static_cast<int>(std::round(previous_w * 100.0)) << "% and " << ratio << "% write: HRT-LI becomes " 
                              << (hrtli_val > comp_val ? "faster" : "slower") << "\n";
                    state = current_state;
                    crossover_ratio = ratio;
                }
                previous_w = w;
            }
            if (crossover_ratio == -1) {
                std::cout << "    No crossover detected across the 0%-100% sweep (HRT-LI remains " << state << ")\n";
            }
        }
    }

    return 0;
}
