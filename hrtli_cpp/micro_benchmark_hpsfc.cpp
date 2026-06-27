/**
 * micro_benchmark_hpsfc.cpp
 * 
 * Micro-benchmark: HP-SFC (O(1) hash) vs std::lower_bound (O(log N) binary search)
 * for hierarchical path key lookup.
 * 
 * Compile (WSL/Linux):
 *   g++ -O3 -std=c++17 -o micro_hpsfc hrtli_cpp/micro_benchmark_hpsfc.cpp
 * 
 * Compile (Windows PowerShell):
 *   g++ -O3 -std=c++17 -o micro_hpsfc.exe hrtli_cpp/micro_benchmark_hpsfc.cpp
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "hrtli_cpp/hpsfc.hpp"

// Generate synthetic hierarchical path strings
std::vector<std::string> generate_paths(int n, int max_depth, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> depth_dist(1, max_depth);
    std::uniform_int_distribution<int> char_dist('a', 'z');
    std::uniform_int_distribution<int> len_dist(2, 8);

    std::vector<std::string> paths;
    paths.reserve(n);
    for (int i = 0; i < n; ++i) {
        int depth = depth_dist(rng);
        std::string path;
        for (int d = 0; d < depth; ++d) {
            path += '/';
            int L = len_dist(rng);
            for (int k = 0; k < L; ++k) {
                path += (char)char_dist(rng);
            }
        }
        paths.push_back(path);
    }
    // Deduplicate and sort
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

int main() {
    const int N = 10000;
    const int N_WARMUP = 1000;
    const int N_REPS = 5;

    auto base_keys = generate_paths(N, 8, 42);
    auto lookup_keys = base_keys;  // 100% hit rate

    // Shuffle for non-sequential access
    std::mt19937 rng(777);
    std::shuffle(lookup_keys.begin(), lookup_keys.end(), rng);

    // Build HP-SFC table
    hrtli::HPSFCTable hpsfc;
    hpsfc.build(base_keys);

    printf("HP-SFC built: %zu keys, table_size=%zu, avg_probes=%.2f\n",
           hpsfc.key_count(), hpsfc.table_size(), hpsfc.avg_probes());

    // Warmup
    volatile int64_t sink = 0;
    for (int i = 0; i < N_WARMUP; ++i) {
        sink += hpsfc.lookup(lookup_keys[i % lookup_keys.size()], base_keys);
    }
    for (int i = 0; i < N_WARMUP; ++i) {
        auto it = std::lower_bound(base_keys.begin(), base_keys.end(),
                                   lookup_keys[i % lookup_keys.size()]);
        sink += (it != base_keys.end() ? 1 : 0);
    }

    // Benchmark HP-SFC
    double hpsfc_best = 1e18;
    for (int r = 0; r < N_REPS; ++r) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (const auto& key : lookup_keys) {
            sink += hpsfc.lookup(key, base_keys);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        hpsfc_best = std::min(hpsfc_best, dt);
    }

    // Benchmark binary search (lower_bound)
    double bs_best = 1e18;
    for (int r = 0; r < N_REPS; ++r) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (const auto& key : lookup_keys) {
            auto it = std::lower_bound(base_keys.begin(), base_keys.end(), key);
            sink += (it != base_keys.end() ? 1 : 0);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double dt = std::chrono::duration<double>(t1 - t0).count();
        bs_best = std::min(bs_best, dt);
    }

    double n_lookups = (double)lookup_keys.size();
    double hpsfc_mops = n_lookups / hpsfc_best / 1e6;
    double bs_mops = n_lookups / bs_best / 1e6;
    double speedup = bs_best / hpsfc_best;

    printf("\n=== HP-SFC vs Binary Search Micro-Benchmark ===\n");
    printf("N=%d keys, N_REPS=%d\n", (int)base_keys.size(), N_REPS);
    printf("HP-SFC:       %.3f Mops/s  (%.2f us/key)\n",
           hpsfc_mops, hpsfc_best * 1e6 / n_lookups);
    printf("Binary search: %.3f Mops/s  (%.2f us/key)\n",
           bs_mops, bs_best * 1e6 / n_lookups);
    printf("Speedup: %.2fx\n\n", speedup);

    (void)sink;  // suppress optimization
    return 0;
}
