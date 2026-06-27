#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <map>
#include <iomanip>
#include <cmath>
#include <fstream>

#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::high_resolution_clock;

double elapsed_us(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::micro>(end - start).count();
}

struct LatencyStats {
    double p50 = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double p999 = 0.0;
    double max_val = 0.0;
};

LatencyStats get_stats(std::vector<double> latencies) {
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    LatencyStats stats;
    if (n > 0) {
        stats.p50 = latencies[n * 0.50];
        stats.p95 = latencies[n * 0.95];
        stats.p99 = latencies[n * 0.99];
        stats.p999 = latencies[n * 0.999];
        stats.max_val = latencies.back();
    }
    return stats;
}

void run_spike_benchmark() {
    std::cout << "\n=============================================" << std::endl;
    std::cout << "Experiment 1: Latency Spike Elimination" << std::endl;
    std::cout << "=============================================" << std::endl;

    // We build on 2,000 base keys, then insert 5,000 keys
    std::vector<std::string> base_keys;
    for (int i = 0; i < 2000; ++i) {
        base_keys.push_back("key_" + std::to_string(i * 10));
    }
    std::sort(base_keys.begin(), base_keys.end());

    std::vector<std::string> insert_keys;
    for (int i = 0; i < 5000; ++i) {
        insert_keys.push_back("key_" + std::to_string(i * 10 + 5));
    }

    // Benchmark v2 (RankTransportIndex)
    std::cout << "Running v2 (RankTransportIndex) insertion trace..." << std::endl;
    hrtli::RankTransportIndex v2_index(base_keys, 1);
    std::vector<double> v2_latencies;
    for (const auto& key : insert_keys) {
        auto start = Clock::now();
        v2_index.insert(key);
        // Periodically trigger consolidation in v2 to simulate background/foreground consolidations
        if (v2_latencies.size() % 1000 == 999) {
            v2_index.maybe_consolidate(0.5);
            v2_index.wait_rebuild();
        }
        auto end = Clock::now();
        v2_latencies.push_back(elapsed_us(start, end));
    }

    // Benchmark v3 (v3::KineticSegmentTree)
    std::cout << "Running v3 (v3::KineticSegmentTree) insertion trace..." << std::endl;
    hrtli::v3::KineticSegmentTree v3_index(8.0, 32);
    for (const auto& key : base_keys) {
        v3_index.insert(key);
    }
    std::vector<double> v3_latencies;
    for (const auto& key : insert_keys) {
        auto start = Clock::now();
        v3_index.insert(key);
        auto end = Clock::now();
        v3_latencies.push_back(elapsed_us(start, end));
    }

    auto v2_stats = get_stats(v2_latencies);
    auto v3_stats = get_stats(v3_latencies);

    // Write traces to CSV
    std::ofstream csv("results/latency_v2_v3.csv");
    csv << "step,v2_latency_us,v3_latency_us\n";
    for (size_t i = 0; i < insert_keys.size(); ++i) {
        csv << i << "," << v2_latencies[i] << "," << v3_latencies[i] << "\n";
    }
    csv.close();
    std::cout << "Wrote latency traces to results/latency_v2_v3.csv" << std::endl;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "v2 Latency (us):"
              << "\n  p50:   " << v2_stats.p50
              << "\n  p95:   " << v2_stats.p95
              << "\n  p99:   " << v2_stats.p99
              << "\n  p99.9: " << v2_stats.p999
              << "\n  Max:   " << v2_stats.max_val << std::endl;

    std::cout << "v3 Latency (us):"
              << "\n  p50:   " << v3_stats.p50
              << "\n  p95:   " << v3_stats.p95
              << "\n  p99:   " << v3_stats.p99
              << "\n  p99.9: " << v3_stats.p999
              << "\n  Max:   " << v3_stats.max_val << std::endl;

    std::cout << "\nSpike Comparison: v2 Max/p50 ratio: " << (v2_stats.max_val / v2_stats.p50)
              << ", v3 Max/p50 ratio: " << (v3_stats.max_val / v3_stats.p50) << std::endl;
}

void run_drift_benchmark() {
    std::cout << "\n=============================================" << std::endl;
    std::cout << "Experiment 2: OOD Skewed Write Drift" << std::endl;
    std::cout << "=============================================" << std::endl;

    // Target concentrated inserts inside a narrow region: "/prefix/event_99XXXX"
    std::vector<std::string> base_keys;
    for (int i = 0; i < 2000; ++i) {
        base_keys.push_back("/prefix/event_" + std::to_string(i));
    }
    std::sort(base_keys.begin(), base_keys.end());

    // v3 Index build
    hrtli::v3::KineticSegmentTree v3_index(4.0, 16);
    for (const auto& key : base_keys) {
        v3_index.insert(key);
    }

    std::cout << "Inserting 3,000 concentrated skewed OOD keys into a single leaf range..." << std::endl;
    auto start = Clock::now();
    for (int i = 0; i < 3000; ++i) {
        v3_index.insert("/prefix/event_1000_" + std::to_string(i));
    }
    auto end = Clock::now();

    std::cout << "Time to insert 3,000 skewed keys: " << elapsed_us(start, end) / 1000.0 << " ms" << std::endl;
    std::cout << "Invariant check after OOD skew: " << (v3_index.validate_invariants() ? "PASS" : "FAIL") << std::endl;
}

void run_throughput_benchmark() {
    std::cout << "\n=============================================" << std::endl;
    std::cout << "Experiment 3: Throughput Characterization" << std::endl;
    std::cout << "=============================================" << std::endl;

    std::vector<std::string> keys;
    std::mt19937 rng(1337);
    for (int i = 0; i < 10000; ++i) {
        keys.push_back("key_" + std::to_string(rng() % 50000));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    // 1. Std::map
    {
        std::map<std::string, int> baseline;
        auto start = Clock::now();
        for (size_t i = 0; i < keys.size(); ++i) {
            baseline[keys[i]] = i;
        }
        auto end = Clock::now();
        double ms = elapsed_us(start, end) / 1000.0;
        std::cout << "std::map insert throughput: " << (keys.size() / ms) / 1000.0 << " MOps/sec" << std::endl;

        start = Clock::now();
        volatile int dummy = 0;
        for (const auto& key : keys) {
            dummy += baseline[key];
        }
        end = Clock::now();
        ms = elapsed_us(start, end) / 1000.0;
        std::cout << "std::map lookup throughput: " << (keys.size() / ms) / 1000.0 << " MOps/sec" << std::endl;
    }

    // 2. v3 KineticSegmentTree
    {
        hrtli::v3::KineticSegmentTree v3_index(16.0, 64);
        auto start = Clock::now();
        for (const auto& key : keys) {
            v3_index.insert(key);
        }
        auto end = Clock::now();
        double ms = elapsed_us(start, end) / 1000.0;
        std::cout << "v3 CKST insert throughput:  " << (keys.size() / ms) / 1000.0 << " MOps/sec" << std::endl;

        start = Clock::now();
        volatile uint64_t dummy = 0;
        for (const auto& key : keys) {
            dummy += v3_index.rank_lookup(key);
        }
        end = Clock::now();
        ms = elapsed_us(start, end) / 1000.0;
        std::cout << "v3 CKST lookup throughput:  " << (keys.size() / ms) / 1000.0 << " MOps/sec" << std::endl;
    }
}

int main() {
    run_spike_benchmark();
    run_drift_benchmark();
    run_throughput_benchmark();
    return 0;
}
