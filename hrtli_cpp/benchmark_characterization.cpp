#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <iomanip>
#include <cmath>

#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

std::vector<std::string> characterization_read_keys(const std::string& path, size_t limit = 0) {
    std::ifstream file(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            keys.push_back(line);
            if (limit > 0 && keys.size() >= limit) {
                break;
            }
        }
    }
    return keys;
}

void characterization_sorted_unique(std::vector<std::string>& keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
}

// 2. Lookup Latency as a Function of m/n
void run_latency_vs_mn() {
    std::cout << "--- Running Lookup Latency vs m/n ---" << std::endl;
    std::vector<std::string> base_keys = characterization_read_keys("data/synthetic_initial.txt");
    if (base_keys.empty()) {
        std::cerr << "Error: synthetic initial dataset empty" << std::endl;
        return;
    }
    characterization_sorted_unique(base_keys);
    
    std::vector<double> ratios = {0.0, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5};
    std::vector<double> v2_point_latencies_ns;
    std::vector<double> v2_rank_latencies_ns;
    std::vector<double> v3_point_latencies_ns;
    std::vector<double> v3_rank_latencies_ns;
    
    for (double r : ratios) {
        std::cout << "Testing ratio " << r << "..." << std::endl;
        hrtli::RankTransportIndex index(base_keys, 64);
        hrtli::RankTransportIndexV3 index_v3(base_keys, 64);

        size_t num_inserts = std::round(base_keys.size() * r);
        std::vector<std::string> inserted_keys;
        inserted_keys.reserve(num_inserts);
        for (size_t i = 0; i < num_inserts; ++i) {
            std::string key = "/root/insert_mn/" + std::to_string(i);
            index.insert(key);
            index_v3.insert(key);
            inserted_keys.push_back(key);
        }
        
        // Prepare lookup pool: mix of base and inserted keys
        std::vector<std::string> lookup_pool = base_keys;
        lookup_pool.insert(lookup_pool.end(), inserted_keys.begin(), inserted_keys.end());
        
        std::mt19937 rng(42);
        std::shuffle(lookup_pool.begin(), lookup_pool.end(), rng);
        
        // Perform 100,000 lookups
        size_t num_lookups = 100000;
        auto point_start = std::chrono::high_resolution_clock::now();
        volatile int point_sink = 0;
        for (size_t i = 0; i < num_lookups; ++i) {
            point_sink += (index.point_lookup(lookup_pool[i % lookup_pool.size()]) ? 1 : 0);
        }
        auto point_end = std::chrono::high_resolution_clock::now();
        (void)point_sink;

        auto rank_start = std::chrono::high_resolution_clock::now();
        volatile int rank_sink = 0;
        for (size_t i = 0; i < num_lookups; ++i) {
            rank_sink += index.lookup(lookup_pool[i % lookup_pool.size()]);
        }
        auto rank_end = std::chrono::high_resolution_clock::now();
        (void)rank_sink;

        auto v3_point_start = std::chrono::high_resolution_clock::now();
        volatile int v3_point_sink = 0;
        for (size_t i = 0; i < num_lookups; ++i) {
            v3_point_sink += (index_v3.point_lookup(lookup_pool[i % lookup_pool.size()]) ? 1 : 0);
        }
        auto v3_point_end = std::chrono::high_resolution_clock::now();
        (void)v3_point_sink;

        auto v3_rank_start = std::chrono::high_resolution_clock::now();
        volatile int v3_rank_sink = 0;
        for (size_t i = 0; i < num_lookups; ++i) {
            v3_rank_sink += index_v3.lookup(lookup_pool[i % lookup_pool.size()]);
        }
        auto v3_rank_end = std::chrono::high_resolution_clock::now();
        (void)v3_rank_sink;

        double point_avg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(point_end - point_start).count() / static_cast<double>(num_lookups);
        double rank_avg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(rank_end - rank_start).count() / static_cast<double>(num_lookups);
        double v3_point_avg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(v3_point_end - v3_point_start).count() / static_cast<double>(num_lookups);
        double v3_rank_avg_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(v3_rank_end - v3_rank_start).count() / static_cast<double>(num_lookups);
        
        v2_point_latencies_ns.push_back(point_avg_ns);
        v2_rank_latencies_ns.push_back(rank_avg_ns);
        v3_point_latencies_ns.push_back(v3_point_avg_ns);
        v3_rank_latencies_ns.push_back(v3_rank_avg_ns);
        
        std::cout << "Ratio: " << r 
                  << ", V2 Point: " << point_avg_ns << " ns, V2 Rank: " << rank_avg_ns << " ns"
                  << ", V3 Point: " << v3_point_avg_ns << " ns, V3 Rank: " << v3_rank_avg_ns << " ns" << std::endl;
    }
    
    std::ofstream file("results_q1/latency_vs_mn.json");
    file << "{\n  \"ratios\": [";
    for (size_t i = 0; i < ratios.size(); ++i) {
        file << ratios[i] << (i + 1 == ratios.size() ? "" : ", ");
    }
    file << "],\n  \"point_latencies_ns\": [";
    for (size_t i = 0; i < v2_point_latencies_ns.size(); ++i) {
        file << v2_point_latencies_ns[i] << (i + 1 == v2_point_latencies_ns.size() ? "" : ", ");
    }
    file << "],\n  \"rank_latencies_ns\": [";
    for (size_t i = 0; i < v2_rank_latencies_ns.size(); ++i) {
        file << v2_rank_latencies_ns[i] << (i + 1 == v2_rank_latencies_ns.size() ? "" : ", ");
    }
    file << "],\n  \"v2_point_latencies_ns\": [";
    for (size_t i = 0; i < v2_point_latencies_ns.size(); ++i) {
        file << v2_point_latencies_ns[i] << (i + 1 == v2_point_latencies_ns.size() ? "" : ", ");
    }
    file << "],\n  \"v2_rank_latencies_ns\": [";
    for (size_t i = 0; i < v2_rank_latencies_ns.size(); ++i) {
        file << v2_rank_latencies_ns[i] << (i + 1 == v2_rank_latencies_ns.size() ? "" : ", ");
    }
    file << "],\n  \"v3_point_latencies_ns\": [";
    for (size_t i = 0; i < v3_point_latencies_ns.size(); ++i) {
        file << v3_point_latencies_ns[i] << (i + 1 == v3_point_latencies_ns.size() ? "" : ", ");
    }
    file << "],\n  \"v3_rank_latencies_ns\": [";
    for (size_t i = 0; i < v3_rank_latencies_ns.size(); ++i) {
        file << v3_rank_latencies_ns[i] << (i + 1 == v3_rank_latencies_ns.size() ? "" : ", ");
    }
    file << "]\n}\n";
    std::cout << "Saved to results_q1/latency_vs_mn.json" << std::endl;
}

// 3. Memory Footprint Breakdown
void run_memory_breakdown() {
    std::cout << "--- Running Memory Breakdown ---" << std::endl;
    std::vector<std::string> datasets = {
        "synthetic", "filesystem", "url", "dns", "json", "package", "wiki_ts", "osm_cellids"
    };
    
    std::ofstream file("results_q1/memory_breakdown.json");
    file << "{\n  \"datasets\": {\n";
    
    for (size_t d = 0; d < datasets.size(); ++d) {
        const auto& ds = datasets[d];
        std::cout << "Processing dataset " << ds << "..." << std::endl;
        
        // Limit read length for large datasets to prevent OOM
        size_t limit_initial = (ds == "wiki_ts" || ds == "osm_cellids") ? 200000 : 0;
        size_t limit_inserts = (ds == "wiki_ts" || ds == "osm_cellids") ? 50000 : 0;
        
        std::vector<std::string> initial = characterization_read_keys("data/" + ds + "_initial.txt", limit_initial);
        std::vector<std::string> inserts = characterization_read_keys("data/" + ds + "_insert.txt", limit_inserts);
        
        characterization_sorted_unique(initial);
        
        hrtli::RankTransportIndexV3 index(initial, 64);
        initial.clear();
        initial.shrink_to_fit();
        
        for (const auto& key : inserts) {
            index.insert(key);
        }
        inserts.clear();
        inserts.shrink_to_fit();
        
        size_t base_keys_sz = 0;
        size_t model_sz = 0;
        size_t hpsfc_sz = 0;
        size_t delta_sz = 0;
        index.get_memory_breakdown(base_keys_sz, model_sz, hpsfc_sz, delta_sz);
        
        size_t total_keys = index.size();
        double base_array_b = static_cast<double>(base_keys_sz) / total_keys;
        double model_segments_b = static_cast<double>(model_sz) / total_keys;
        double hpsfc_table_b = static_cast<double>(hpsfc_sz) / total_keys;
        double delta_nodes_b = static_cast<double>(delta_sz) / total_keys;
        
        file << "    \"" << ds << "\": {\n"
             << "      \"base_array\": " << base_array_b << ",\n"
             << "      \"model_segments\": " << model_segments_b << ",\n"
             << "      \"hpsfc_table\": " << hpsfc_table_b << ",\n"
             << "      \"delta_nodes\": " << delta_nodes_b << "\n"
             << "    }" << (d + 1 == datasets.size() ? "" : ",") << "\n";
             
        std::cout << "Dataset: " << ds << " | Base: " << base_array_b << " B/key | Model: " << model_segments_b
                  << " B/key | HPSFC: " << hpsfc_table_b << " B/key | Delta: " << delta_nodes_b << " B/key" << std::endl;
    }
    
    file << "  }\n}\n";
    std::cout << "Saved to results_q1/memory_breakdown.json" << std::endl;
}

// 4. Model Evolution Across Generations
void run_model_evolution() {
    std::cout << "--- Running Model Evolution ---" << std::endl;
    std::vector<std::string> base_keys = characterization_read_keys("data/synthetic_initial.txt");
    characterization_sorted_unique(base_keys);
    
    hrtli::RankTransportIndexV3 index(base_keys, 64);
    
    int num_generations = 15;
    std::vector<int> generations;
    std::vector<size_t> num_segments;
    std::vector<int> max_errors;
    
    for (int g = 1; g <= num_generations; ++g) {
        std::cout << "Generation " << g << "..." << std::endl;
        // Insert 500 out-of-distribution keys
        for (int i = 0; i < 500; ++i) {
            std::string key = "/root/gen_" + std::to_string(g) + "/key_" + std::to_string(i);
            index.insert(key);
        }
        
        // Consolidate index
        index.consolidate();
        
        size_t segs = index.model_segments_count();
        int max_err = index.get_model_max_error();
        
        generations.push_back(g);
        num_segments.push_back(segs);
        max_errors.push_back(max_err);
        
        std::cout << "Gen: " << g << ", Segments: " << segs << ", Max Error: " << max_err << std::endl;
    }
    
    std::ofstream file("results_q1/model_evolution.json");
    file << "{\n  \"generations\": [";
    for (size_t i = 0; i < generations.size(); ++i) {
        file << generations[i] << (i + 1 == generations.size() ? "" : ", ");
    }
    file << "],\n  \"num_segments\": [";
    for (size_t i = 0; i < num_segments.size(); ++i) {
        file << num_segments[i] << (i + 1 == num_segments.size() ? "" : ", ");
    }
    file << "],\n  \"max_errors\": [";
    for (size_t i = 0; i < max_errors.size(); ++i) {
        file << max_errors[i] << (i + 1 == max_errors.size() ? "" : ", ");
    }
    file << "]\n}\n";
    std::cout << "Saved to results_q1/model_evolution.json" << std::endl;
}

int main() {
    run_latency_vs_mn();
    run_memory_breakdown();
    run_model_evolution();
    return 0;
}
