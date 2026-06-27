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

#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

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

struct ThreadResult {
    std::vector<std::vector<uint64_t>> bucket_latencies;
    ThreadResult() {
        bucket_latencies.resize(120); // up to 12 seconds of 100ms buckets
        for (auto& v : bucket_latencies) {
            v.reserve(100000);
        }
    }
};

template <typename IndexType>
void reader_work(int thread_id, IndexType& index, const std::vector<std::string>& lookup_pool,
                 std::chrono::high_resolution_clock::time_point start_time,
                 std::atomic<bool>& stop_flag, ThreadResult& result) {
    std::mt19937 rng(1337 + thread_id);
    std::uniform_int_distribution<size_t> dist(0, lookup_pool.size() - 1);
    
    size_t count = 0;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        size_t idx = dist(rng);
        const std::string& key = lookup_pool[idx];
        
        auto t_before = std::chrono::high_resolution_clock::now();
        volatile int rank = index.lookup(key);
        auto t_after = std::chrono::high_resolution_clock::now();
        (void)rank;
        
        // Sample every 20th lookup to keep overhead low and distribute uniformly
        if (count++ % 20 == 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t_before - start_time).count();
            int bucket = elapsed / 100;
            if (bucket >= 0 && bucket < 120) {
                auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_after - t_before).count();
                if (result.bucket_latencies[bucket].size() < 100000) {
                    result.bucket_latencies[bucket].push_back(latency_ns);
                }
            }
        }
    }
}

template <typename IndexType>
void writer_work(IndexType& index, std::atomic<bool>& stop_flag) {
    int key_counter = 0;
    while (!stop_flag.load(std::memory_order_relaxed)) {
        // Insert a batch of keys
        for (int i = 0; i < 200; ++i) {
            std::string key = "/root/inserted_concurrency_" + std::to_string(key_counter++);
            index.insert(key);
        }
        
        // Trigger consolidation
        index.consolidate();
        
        // Sleep for 600ms
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
    }
}

template <typename IndexType>
void run_concurrent_benchmark(const std::vector<std::string>& initial_keys, const std::string& index_name, const std::string& output_path) {
    std::cout << "Initializing " << index_name << " with " << initial_keys.size() << " keys..." << std::endl;
    IndexType index(initial_keys, 64);
    
    std::atomic<bool> stop_flag(false);
    
    std::cout << "Starting reader and writer threads..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Spawn writer thread
    std::thread writer(writer_work<IndexType>, std::ref(index), std::ref(stop_flag));
    
    // Spawn reader threads
    const int num_readers = 4;
    std::vector<std::thread> readers;
    std::vector<ThreadResult> thread_results(num_readers);
    for (int i = 0; i < num_readers; ++i) {
        readers.emplace_back(reader_work<IndexType>, i, std::ref(index), std::ref(initial_keys),
                             start_time, std::ref(stop_flag), std::ref(thread_results[i]));
    }
    
    // Let benchmark run for 8 seconds
    std::cout << "Running benchmark for 8 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(8));
    
    std::cout << "Stopping benchmark..." << std::endl;
    stop_flag.store(true);
    
    writer.join();
    for (auto& r : readers) {
        r.join();
    }
    
    // Ensure final background rebuild is joined
    index.wait_rebuild();
    
    std::cout << "Benchmark complete. Total consolidations triggered: " << index.consolidation_count() << std::endl;
    
    // Process results
    std::ofstream json_out(output_path);
    json_out << "{\n  \"timeline\": [\n";
    
    bool first = true;
    for (int b = 0; b < 120; ++b) {
        std::vector<uint64_t> all_latencies;
        for (int t = 0; t < num_readers; ++t) {
            all_latencies.insert(all_latencies.end(),
                                 thread_results[t].bucket_latencies[b].begin(),
                                 thread_results[t].bucket_latencies[b].end());
        }
        if (all_latencies.empty()) {
            continue;
        }
        std::sort(all_latencies.begin(), all_latencies.end());
        size_t idx = static_cast<size_t>(all_latencies.size() * 0.999);
        if (idx >= all_latencies.size()) idx = all_latencies.size() - 1;
        uint64_t p99_9 = all_latencies[idx];
        
        if (!first) {
            json_out << ",\n";
        }
        json_out << "    {\"time_ms\": " << b * 100 << ", \"p99_9_latency_ns\": " << p99_9 << "}";
        first = false;
    }
    json_out << "\n  ]\n}\n";
    std::cout << "Results written to " << output_path << std::endl;
}

int main() {
    std::cout << "Loading synthetic dataset..." << std::endl;
    std::vector<std::string> initial_keys = read_keys("data/synthetic_initial.txt");
    if (initial_keys.empty()) {
        std::cerr << "Error: synthetic dataset not found or empty." << std::endl;
        return 1;
    }
    
    // Run V2, write to results_q1/concurrent_timeline_v2.json
    run_concurrent_benchmark<hrtli::RankTransportIndex>(initial_keys, "RankTransportIndex (V2)", "results_q1/concurrent_timeline_v2.json");
    
    // Run V3, write to results_q1/concurrent_timeline.json
    run_concurrent_benchmark<hrtli::RankTransportIndexV3>(initial_keys, "RankTransportIndexV3 (V3)", "results_q1/concurrent_timeline.json");
    
    return 0;
}
