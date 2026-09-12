#include <cassert>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "rank_transport.hpp"

int main() {
    hrtli::CountedBTreeDelta delta;
    assert(delta.mark_inserted("/b"));
    assert(delta.mark_inserted("/d"));
    assert(delta.mark_deleted("/c"));
    assert(delta.prefix_delta("/a") == 0);
    assert(delta.prefix_delta("/c") == 1);
    assert(delta.prefix_delta("/e") == 1);
    assert(!delta.mark_inserted("/b"));
    assert(delta.mark_deleted("/b"));
    assert(delta.inserted_size() == 1);
    assert(delta.deleted_size() == 2);
    assert(delta.prefix_delta("/c") == -1);
    assert(delta.discard_deleted("/b"));
    assert(delta.mutation_count() == 2);
    assert(delta.prefix_delta("/e") == 0);
    assert(delta.validate_internal_lcps());

    hrtli::CountedBTreeDelta many_delta;
    std::map<std::string, int> oracle;
    std::vector<std::string> many_keys;
    for (int i = 0; i < 160; ++i) {
        many_keys.push_back("/shared/prefix/key_" + std::to_string((i * 37) % 997));
    }
    std::sort(many_keys.begin(), many_keys.end());
    many_keys.erase(std::unique(many_keys.begin(), many_keys.end()), many_keys.end());
    for (size_t i = 0; i < many_keys.size(); ++i) {
        int weight = (i % 3 == 0) ? -1 : 1;
        if (weight > 0) {
            assert(many_delta.mark_inserted(many_keys[i]));
        } else {
            assert(many_delta.mark_deleted(many_keys[i]));
        }
        oracle[many_keys[i]] = weight;
    }
    assert(many_delta.validate_internal_lcps());
    for (size_t i = 0; i < many_keys.size(); i += 5) {
        if (oracle[many_keys[i]] > 0) {
            assert(many_delta.discard_inserted(many_keys[i]));
        } else {
            assert(many_delta.discard_deleted(many_keys[i]));
        }
        oracle.erase(many_keys[i]);
    }
    assert(many_delta.validate_internal_lcps());
    for (const auto& probe : many_keys) {
        int before = 0;
        int before_or_equal = 0;
        for (const auto& entry : oracle) {
            if (entry.first < probe) before += entry.second;
            if (entry.first <= probe) before_or_equal += entry.second;
        }
        assert(many_delta.prefix_delta(probe) == before);
        assert(many_delta.prefix_delta_le(probe) == before_or_equal);
    }

    std::vector<std::string> base = {"/a", "/c", "/e", "/g"};
    hrtli::RankTransportIndex index(base, 1);

    assert(index.size() == 4);
    assert(index.lookup("/c") == 1);

    assert(index.insert("/b"));
    assert(index.insert("/f"));
    assert(index.remove("/c"));
    assert(index.point_lookup("/b"));
    assert(index.point_lookup("/f"));
    assert(!index.point_lookup("/c"));
    assert(index.lookup("/c") == -1);
    assert(index.exact_rank("/b") == 1);
    assert(index.exact_rank("/e") == 2);
    assert(std::abs(index.transported_predict("/a") - index.exact_rank("/a")) <= index.certified_epsilon());
    assert(std::abs(index.transported_predict("/e") - index.exact_rank("/e")) <= index.certified_epsilon());
    assert(std::abs(index.transported_predict("/g") - index.exact_rank("/g")) <= index.certified_epsilon());
    bool inserted_prediction_rejected = false;
    try {
        (void)index.transported_predict("/b");
    } catch (const std::runtime_error&) {
        inserted_prediction_rejected = true;
    }
    assert(inserted_prediction_rejected);
    bool deleted_prediction_rejected = false;
    try {
        (void)index.transported_predict("/c");
    } catch (const std::runtime_error&) {
        deleted_prediction_rejected = true;
    }
    assert(deleted_prediction_rejected);
    assert(index.mutation_count() == 3);
    assert(index.count_range("/a", "/f") == 4);
    std::vector<std::string> expected_scan = {"/a", "/b", "/e", "/f"};
    assert(index.scan_range("/a", "/f") == expected_scan);

    auto before = index.snapshot_keys();
    std::vector<std::string> expected_before = {"/a", "/b", "/e", "/f", "/g"};
    assert(before == expected_before);

    assert(index.maybe_consolidate(0.5));
    index.wait_rebuild();
    assert(index.mutation_count() == 0);
    assert(index.consolidation_count() == 1);
    assert(index.size() == 5);
    assert(index.lookup("/b") == 1);
    assert(index.lookup("/c") == -1);
    assert(index.exact_rank("/g") == 4);
    assert(std::abs(index.transported_predict("/b") - index.exact_rank("/b")) <= index.certified_epsilon());

    assert(!index.maybe_consolidate(0.5));
    assert(index.insert("/c"));
    assert(index.lookup("/c") == 2);

    // Test concurrent writes/lookups during active background rebuild
    std::vector<std::string> base_concurrent = {"/1", "/3", "/5", "/7"};
    hrtli::RankTransportIndex idx_conc(base_concurrent, 1);
    assert(idx_conc.insert("/2"));
    assert(idx_conc.insert("/4"));
    assert(idx_conc.maybe_consolidate(0.5));
    // Rebuild is running in the background. Write and read concurrently:
    assert(idx_conc.insert("/6"));
    assert(idx_conc.contains("/6"));
    assert(idx_conc.contains("/2"));
    idx_conc.wait_rebuild();
    // After rebuild, all keys should be live, but only /6 is in delta layer
    assert(idx_conc.contains("/6"));
    assert(idx_conc.contains("/2"));
    assert(idx_conc.contains("/4"));
    assert(idx_conc.mutation_count() == 1);

    std::vector<std::string> long_prefix;
    for (int i = 0; i < 128; ++i) {
        long_prefix.push_back("/very/long/common/prefix/that/collides/in/the/old/float/model/key_" + std::to_string(i));
    }
    hrtli::RankTransportIndex long_index(long_prefix, 1);
    assert(long_index.insert("/very/long/common/prefix/that/collides/in/the/old/float/model/key_064/child"));
    for (const auto& key : long_prefix) {
        assert(long_index.lookup(key) == long_index.exact_rank(key));
    }

    // Monotonicity test for segment_of()
    {
        std::vector<std::string> mono_keys;
        for (int i = 0; i < 200; ++i) {
            mono_keys.push_back("/mono/key_" + std::to_string((i * 17) % 500));
        }
        std::sort(mono_keys.begin(), mono_keys.end());
        mono_keys.erase(std::unique(mono_keys.begin(), mono_keys.end()), mono_keys.end());

        hrtli::PiecewiseLinearModel model;
        // Build the model with a few different epsilon values
        for (int eps : {1, 2, 4, 8}) {
            model.build(mono_keys, eps);
            for (size_t i = 0; i < mono_keys.size(); ++i) {
                for (size_t j = i; j < mono_keys.size(); ++j) {
                    int seg_i = model.segment_of(mono_keys[i]);
                    int seg_j = model.segment_of(mono_keys[j]);
                    assert(seg_i <= seg_j);
                }
            }
        }
    }

    // --- Phase 3 Write Buffer & 3-Way Merge Tests ---
    {
        // 1. Write buffer capacity and flushing
        hrtli::SegmentedDelta<hrtli::CountedBTreeDelta> seg_delta(3);
        
        assert(seg_delta.get_write_buffer().empty());
        assert(seg_delta.size() == 0);

        for (int i = 0; i < 31; ++i) {
            std::string k = "/k_" + std::to_string(i + 100);
            assert(seg_delta.mark_inserted(k, 1));
        }
        assert(seg_delta.get_write_buffer().size() == 31);
        assert(seg_delta.size() == 31);
        assert(seg_delta.inserted_list().empty());

        assert(seg_delta.mark_inserted("/k_131", 1));
        assert(seg_delta.get_write_buffer().empty());
        assert(seg_delta.inserted_list().size() == 32);
        assert(seg_delta.size() == 32);

        // 2. Prefix delta and prefix_delta_le on write buffer
        hrtli::SegmentedDelta<hrtli::CountedBTreeDelta> seg_delta2(3);
        assert(seg_delta2.mark_inserted("/b", 1));
        assert(seg_delta2.mark_inserted("/d", 1));
        assert(seg_delta2.mark_deleted("/c", 1));

        assert(seg_delta2.prefix_delta("/a", 1) == 0);
        assert(seg_delta2.prefix_delta("/c", 1) == 1);
        assert(seg_delta2.prefix_delta("/d", 1) == 0);
        assert(seg_delta2.prefix_delta("/e", 1) == 1);

        assert(seg_delta2.prefix_delta_le("/b", 1) == 1);
        assert(seg_delta2.prefix_delta_le("/c", 1) == 0);
        assert(seg_delta2.prefix_delta_le("/d", 1) == 1);

        // 3. 3-Way merge range scan
        std::vector<std::string> base = {"/a", "/c", "/e"};
        hrtli::RankTransportIndex index(base, 1);

        assert(index.insert("/b"));
        assert(index.remove("/e"));
        
        size_t b, m, h, d;
        index.get_memory_breakdown(b, m, h, d); // flushes the write buffer

        assert(index.insert("/d"));
        assert(index.remove("/c"));

        std::vector<std::string> expected = {"/a", "/b", "/d"};
        assert(index.scan_range("/a", "/z") == expected);
    }

    // --- Phase 5: ApproxLocalDelta Tests ---
    {
        // 1. Instantiation and basic operations
        hrtli::SegmentedDelta<hrtli::ApproxLocalDelta> approx_seg_delta(4);
        assert(approx_seg_delta.size() == 0);
        assert(approx_seg_delta.empty());

        // Insert key into segment 0
        assert(approx_seg_delta.mark_inserted("/a", 0, 0.0, 0.25));
        // Insert key into segment 1
        assert(approx_seg_delta.mark_inserted("/c", 1, 0.25, 0.5));
        // Insert key into segment 2
        assert(approx_seg_delta.mark_inserted("/e", 2, 0.5, 0.75));
        // Delete key in segment 1
        assert(approx_seg_delta.mark_deleted("/d", 1, 0.25, 0.5));

        // Flush write buffer
        approx_seg_delta.flush();

        // Check sizes
        assert(approx_seg_delta.size() == 4);
        assert(approx_seg_delta.inserted_size() == 3);
        assert(approx_seg_delta.deleted_size() == 1);

        // Check lookups
        assert(approx_seg_delta.contains_inserted("/a", 0));
        assert(approx_seg_delta.contains_inserted("/c", 1));
        assert(approx_seg_delta.contains_inserted("/e", 2));
        assert(approx_seg_delta.contains_deleted("/d", 1));

        // Discard insertion / deletion
        assert(approx_seg_delta.discard_inserted("/c", 1));
        assert(approx_seg_delta.discard_deleted("/d", 1));
        
        approx_seg_delta.flush();

        assert(!approx_seg_delta.contains_inserted("/c", 1));
        assert(!approx_seg_delta.contains_deleted("/d", 1));

        assert(approx_seg_delta.inserted_size() == 2);
        assert(approx_seg_delta.deleted_size() == 0);

        // Check lists
        std::vector<std::string> expected_inserted = {"/a", "/e"};
        assert(approx_seg_delta.inserted_list() == expected_inserted);
        assert(approx_seg_delta.deleted_list().empty());

        // Test prefix sum approximation
        // Let's create an ApproxLocalDelta with many insertions to exceed res_limit
        hrtli::ApproxLocalDelta approx_delta(0.0, 1.0, 4, 2); // num_bins = 4, res_limit = 2
        // res_limit is 2, so first two go to residual buffer: "/b" and "/d"
        assert(approx_delta.mark_inserted("/b"));
        assert(approx_delta.mark_inserted("/d"));
        // Next one goes to bins
        assert(approx_delta.mark_inserted("/z"));
        
        assert(approx_delta.size() == 3);
        assert(approx_delta.contains_inserted("/b"));
        assert(approx_delta.contains_inserted("/d"));
        assert(approx_delta.contains_inserted("/z"));

        // Test prefix_delta
        int p_a = approx_delta.prefix_delta("/a");
        int p_z = approx_delta.prefix_delta("/z");
        // Verify they are within reasonable bounds or exact
        assert(p_a >= 0 && p_a <= 3);
        assert(p_z >= 0 && p_z <= 3);

        // Range iteration
        std::vector<std::pair<std::string, int>> out;
        approx_delta.iter_range("/a", "/z", out);
        assert(out.size() == 3);
        assert(out[0].first == "/b");
        assert(out[1].first == "/d");
        assert(out[2].first == "/z");
    }

    std::cout << "C++ rank transport consolidation tests passed\n";
    return 0;
}
