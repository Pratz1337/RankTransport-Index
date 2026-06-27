#include <cassert>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <set>
#include "kinetic_segment_tree.hpp"

// Brute-force reference set to track active keys
std::vector<std::string> get_sorted_keys(const std::set<std::string>& active_set) {
    std::vector<std::string> sorted(active_set.begin(), active_set.end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

int main() {
    std::cout << "Starting v3 Oracle and Adversarial Stress Test..." << std::endl;

    // Workload 1: Randomized Stress Test (5,000 operations)
    {
        std::cout << "Running Workload 1: Randomized mutations..." << std::endl;
        hrtli::v3::KineticSegmentTree tree(8.0, 32); // Small leaf mass to trigger many splits/merges
        std::set<std::string> active_keys;
        std::vector<std::string> pool;
        
        // Generate a pool of 2,000 random string keys
        std::mt19937 rng(42);
        for (int i = 0; i < 2000; ++i) {
            pool.push_back("key_" + std::to_string(rng() % 10000));
        }

        for (int step = 0; step < 5000; ++step) {
            std::string k = pool[rng() % pool.size()];
            if (rng() % 2 == 0) {
                // Insert
                tree.insert(k);
                active_keys.insert(k);
            } else {
                // Delete
                if (active_keys.count(k)) {
                    tree.remove(k);
                    active_keys.erase(k);
                }
            }

            // Verify invariants and certificates every 100 steps
            if (step % 100 == 0) {
                assert(tree.validate_invariants());
                auto sorted = get_sorted_keys(active_keys);
                for (size_t i = 0; i < sorted.size(); ++i) {
                    uint32_t expected_rank = i;
                    uint32_t actual_rank = tree.rank_lookup(sorted[i]);
                    if (actual_rank != expected_rank) {
                        std::cerr << "Mismatch at index " << i << " key " << sorted[i] 
                                  << ": expected " << expected_rank << ", got " << actual_rank << std::endl;
                        assert(false);
                    }
                    double pred = tree.predict(sorted[i]);
                    double err = std::abs(pred - expected_rank);
                    if (err > 8.0 + 1e-9) {
                        std::cerr << "Prediction error violation at index " << i << " key " << sorted[i] 
                                  << ": pred=" << pred << ", expected=" << expected_rank 
                                  << ", err=" << err << ", epsilon=8.0" << std::endl;
                        assert(false);
                    }
                }
            }
        }
        std::cout << "Workload 1: SUCCESS." << std::endl;
    }

    // Workload 2: Scalarization Collapse Adversarial Test (Risk 1)
    {
        std::cout << "Running Workload 2: Collapsing prefix pathology..." << std::endl;
        // Leaf capacity is 16, error budget is 4.0
        hrtli::v3::KineticSegmentTree tree(4.0, 16);
        std::set<std::string> active_keys;

        // Generate keys sharing LCP + 8 bytes: "/shared/prefix/event_00XXXX"
        std::string common_prefix = "/shared/prefix/event_00";
        for (int i = 0; i < 200; ++i) {
            std::string suffix = std::to_string(i);
            while (suffix.size() < 4) suffix = "0" + suffix;
            std::string k = common_prefix + suffix;
            tree.insert(k);
            active_keys.insert(k);
        }

        // Verify that it built correctly without any infinite split loops
        assert(tree.validate_invariants());
        auto sorted = get_sorted_keys(active_keys);
        for (size_t i = 0; i < sorted.size(); ++i) {
            uint32_t expected_rank = i;
            uint32_t actual_rank = tree.rank_lookup(sorted[i]);
            assert(actual_rank == expected_rank);
            double pred = tree.predict(sorted[i]);
            double err = std::abs(pred - expected_rank);
            if (err > 4.0 + 1e-9) {
                std::cerr << "Prediction error violation in Workload 2: key " << sorted[i] 
                          << ", pred=" << pred << ", expected=" << expected_rank 
                          << ", err=" << err << ", epsilon=4.0" << std::endl;
                assert(false);
            }
        }
        std::cout << "Workload 2: SUCCESS (Fallback mode handles collapse safely)." << std::endl;
    }

    // Workload 3: Delete-Heavy WarpNode Underflow Test (Risk 4)
    {
        std::cout << "Running Workload 3: Deletion-heavy internal node underflows..." << std::endl;
        hrtli::v3::KineticSegmentTree tree(4.0, 16);
        std::set<std::string> active_keys;

        // Build a reasonably large tree
        for (int i = 0; i < 500; ++i) {
            std::string k = "key_" + std::to_string(i);
            tree.insert(k);
            active_keys.insert(k);
        }
        assert(tree.validate_invariants());

        // Delete almost all keys to trigger extensive leaf and internal merging
        auto sorted = get_sorted_keys(active_keys);
        for (size_t i = 0; i < sorted.size() - 5; ++i) {
            tree.remove(sorted[i]);
            active_keys.erase(sorted[i]);
            assert(tree.validate_invariants());
        }

        // Verify remaining
        auto remaining = get_sorted_keys(active_keys);
        for (size_t i = 0; i < remaining.size(); ++i) {
            uint32_t expected_rank = i;
            uint32_t actual_rank = tree.rank_lookup(remaining[i]);
            assert(actual_rank == expected_rank);
            double pred = tree.predict(remaining[i]);
            double err = std::abs(pred - expected_rank);
            if (err > 4.0 + 1e-9) {
                std::cerr << "Prediction error violation in Workload 3: key " << remaining[i] 
                          << ", pred=" << pred << ", expected=" << expected_rank 
                          << ", err=" << err << ", epsilon=4.0" << std::endl;
                assert(false);
            }
        }
        std::cout << "Workload 3: SUCCESS (Internal WarpNode and Leaf underflows merged correctly)." << std::endl;
    }

    std::cout << "All v3 Oracle and Stress Tests PASSED!" << std::endl;
    return 0;
}
