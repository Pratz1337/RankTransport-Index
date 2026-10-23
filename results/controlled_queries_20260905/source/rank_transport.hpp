#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <shared_mutex>
#include <thread>
#include <mutex>
#include "hpsfc.hpp"
#include "counted_btree_delta.hpp"
#include "prefix_radix_delta.hpp"
#include "approx_local_delta.hpp"
#define DeltaLayerType PrefixRadixDelta

// Concurrent rebuild/read paths pay shared_mutex. Single-threaded paper
// benchmarks (and most competitor APIs) leave locks out of the critical path.
// Define HRTLI_CONCURRENT (Makefile concurrency targets) to re-enable locks.
#ifndef HRTLI_CONCURRENT
#define HRTLI_LOCK_SHARED(mu)   ((void)0)
#define HRTLI_LOCK_UNIQUE(mu)   ((void)0)
#define HRTLI_SHARED_GUARD(name, mu)  struct name##_tag {}
#define HRTLI_UNIQUE_GUARD(name, mu)  struct name##_tag {}
#else
#define HRTLI_LOCK_SHARED(mu)   std::shared_lock<std::shared_mutex> hrtli_shared_guard_(mu)
#define HRTLI_LOCK_UNIQUE(mu)   std::unique_lock<std::shared_mutex> hrtli_unique_guard_(mu)
#define HRTLI_SHARED_GUARD(name, mu)  std::shared_lock<std::shared_mutex> name(mu)
#define HRTLI_UNIQUE_GUARD(name, mu)  std::unique_lock<std::shared_mutex> name(mu)
#endif

#ifdef HRTLI_VERIFY_ORACLE
#include <map>
#include <cassert>
#endif

namespace hrtli {

class PiecewiseLinearModel {
public:
    struct Segment {
        double start_key_val;
        double end_key_val;
        double slope;
        double intercept;
    };

    std::vector<Segment> segments;
    int epsilon;

    void build(const std::vector<std::string>& keys, int eps) {
        epsilon = eps;
        segments.clear();
        int n = keys.size();
        if (n == 0) return;

        std::vector<double> key_vals(n);
        for (int i = 0; i < n; ++i) {
            key_vals[i] = hli_key_to_double(keys[i]);
        }

        int start = 0;
        while (start < n) {
            int end = start + 1;
            double s_lower = -std::numeric_limits<double>::infinity();
            double s_upper = std::numeric_limits<double>::infinity();
            
            while (end < n) {
                double dx = key_vals[end] - key_vals[start];
                if (dx > 1e-12) {
                    double slope_min = (end - epsilon - start) / dx;
                    double slope_max = (end + epsilon - start) / dx;
                    double next_lower = std::max(s_lower, slope_min);
                    double next_upper = std::min(s_upper, slope_max);
                    if (next_lower > next_upper) {
                        break;
                    }
                    s_lower = next_lower;
                    s_upper = next_upper;
                } else {
                    if (end - start > epsilon) {
                        break;
                    }
                }
                end++;
            }
            
            int seg_end = end - 1;
            double slope = 0.0;
            double intercept = start;
            if (seg_end > start) {
                double dx = key_vals[seg_end] - key_vals[start];
                if (dx > 1e-12) {
                    if (s_lower <= s_upper && s_lower != -std::numeric_limits<double>::infinity()) {
                        slope = (s_lower + s_upper) / 2.0;
                    } else {
                        slope = (seg_end - start) / dx;
                    }
                    intercept = start - slope * key_vals[start];
                }
            }
            
            segments.push_back({key_vals[start], key_vals[seg_end], slope, intercept});
            start = seg_end + 1;
        }
    }

    int predict(double val) const {
        if (segments.empty()) return 0;
        int lo = 0, hi = segments.size() - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (segments[mid].end_key_val < val) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        const auto& seg = segments[lo];
        return static_cast<int>(seg.slope * val + seg.intercept);
    }

    int segment_of(double val) const {
        if (segments.empty()) return 0;
        int lo = 0, hi = segments.size() - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (segments[mid].end_key_val < val) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
    int segment_of(const std::string& key) const {
        return segment_of(hli_key_to_double(key));
    }
};

template <typename T>
struct LocalDeltaFactory {
    static T* create(double start, double end) { (void)start; (void)end; return new T(); }
};
template <>
struct LocalDeltaFactory<ApproxLocalDelta> {
    static ApproxLocalDelta* create(double start, double end) { return new ApproxLocalDelta(start, end); }
};

template <typename LocalT = CountedBTreeDelta>
class SegmentedDelta {
public:
    struct BufferEntry {
        std::string key;
        int weight;
        int seg_idx;
        bool operator<(const BufferEntry& o) const { return key < o.key; }
    };

private:
    size_t P = 0;
    std::vector<LocalT*> local_deltas;
    std::vector<int> segment_weights;
    std::vector<int> segment_prefix_sums;
    std::vector<BufferEntry> write_buffer;
    // Logical signed weight of every live mutation (buffer + committed).
    // Point membership becomes expected O(1) without walking the B-tree twice.
    std::unordered_map<std::string, int> live_weights_;
    int inserted_count_ = 0;
    int deleted_count_ = 0;
    static constexpr size_t BUFFER_CAPACITY = 32;

    void update_prefix_sums(int start_idx, int diff) {
        for (size_t i = start_idx + 1; i <= P; ++i) {
            segment_prefix_sums[i] += diff;
        }
    }

    void set_live_weight(const std::string& key, int new_w) {
        auto it = live_weights_.find(key);
        int old_w = (it == live_weights_.end()) ? 0 : it->second;
        if (old_w == new_w) return;
        if (old_w > 0) --inserted_count_;
        else if (old_w < 0) --deleted_count_;
        if (new_w == 0) {
            if (it != live_weights_.end()) live_weights_.erase(it);
        } else {
            live_weights_[key] = new_w;
            if (new_w > 0) ++inserted_count_;
            else ++deleted_count_;
        }
    }

public:
    SegmentedDelta() : P(0) {}
    explicit SegmentedDelta(size_t num_segments) { init(num_segments); }
    ~SegmentedDelta() { clear(); }

    SegmentedDelta(SegmentedDelta&& o) noexcept
        : P(o.P), local_deltas(std::move(o.local_deltas)),
          segment_weights(std::move(o.segment_weights)),
          segment_prefix_sums(std::move(o.segment_prefix_sums)),
          write_buffer(std::move(o.write_buffer)),
          live_weights_(std::move(o.live_weights_)),
          inserted_count_(o.inserted_count_),
          deleted_count_(o.deleted_count_) {
        o.P = 0;
        o.inserted_count_ = 0;
        o.deleted_count_ = 0;
    }

    SegmentedDelta& operator=(SegmentedDelta&& o) noexcept {
        if (this != &o) {
            clear();
            P = o.P;
            local_deltas = std::move(o.local_deltas);
            segment_weights = std::move(o.segment_weights);
            segment_prefix_sums = std::move(o.segment_prefix_sums);
            write_buffer = std::move(o.write_buffer);
            live_weights_ = std::move(o.live_weights_);
            inserted_count_ = o.inserted_count_;
            deleted_count_ = o.deleted_count_;
            o.P = 0;
            o.inserted_count_ = 0;
            o.deleted_count_ = 0;
        }
        return *this;
    }

    SegmentedDelta(const SegmentedDelta&) = delete;
    SegmentedDelta& operator=(const SegmentedDelta&) = delete;

    void init(size_t num_segments) {
        clear();
        P = num_segments > 0 ? num_segments : 1;
        local_deltas.assign(P, nullptr);
        segment_weights.assign(P, 0);
        segment_prefix_sums.assign(P + 1, 0);
        live_weights_.clear();
        inserted_count_ = 0;
        deleted_count_ = 0;
    }

    void clear() {
        for (auto* ptr : local_deltas) delete ptr;
        local_deltas.clear();
        segment_weights.clear();
        segment_prefix_sums.clear();
        write_buffer.clear();
        live_weights_.clear();
        inserted_count_ = 0;
        deleted_count_ = 0;
        P = 0;
    }

    void reset() {
        write_buffer.clear();
        live_weights_.clear();
        inserted_count_ = 0;
        deleted_count_ = 0;
        init(P);
    }

    void flush() {
        if (write_buffer.empty()) return;
        for (const auto& entry : write_buffer) {
            int seg_idx = entry.seg_idx;
            if (!local_deltas[seg_idx]) {
                local_deltas[seg_idx] = LocalDeltaFactory<LocalT>::create(0.0, 1.0);
            }
            int old_w = local_deltas[seg_idx]->contains_inserted(entry.key) ? 1 : (local_deltas[seg_idx]->contains_deleted(entry.key) ? -1 : 0);
            
            bool success = false;
            if (entry.weight > 0) {
                if (old_w == -1) {
                    success = local_deltas[seg_idx]->discard_deleted(entry.key);
                } else if (old_w == 0) {
                    success = local_deltas[seg_idx]->mark_inserted(entry.key);
                }
            } else if (entry.weight < 0) {
                if (old_w == 1) {
                    success = local_deltas[seg_idx]->discard_inserted(entry.key);
                } else if (old_w == 0) {
                    success = local_deltas[seg_idx]->mark_deleted(entry.key);
                }
            }
            if (success) {
                int new_w = old_w + entry.weight;
                int diff = new_w - old_w;
                segment_weights[seg_idx] += diff;
                update_prefix_sums(seg_idx, diff);
            }
        }
        write_buffer.clear();
    }

    const std::vector<BufferEntry>& get_write_buffer() const { return write_buffer; }

    int inserted_size() const { return inserted_count_; }

    int deleted_size() const { return deleted_count_; }

    int size() const { return inserted_count_ + deleted_count_; }

    int mutation_count() const { return size(); }
    bool empty() const { return live_weights_.empty(); }

    /** Signed live weight of key: +1 insert, -1 delete, 0 absent. O(1) expected. */
    int weight_of(const std::string& key) const {
        auto it = live_weights_.find(key);
        return (it == live_weights_.end()) ? 0 : it->second;
    }

    size_t memory_bytes() const {
        size_t total = sizeof(*this);
        total += local_deltas.capacity() * sizeof(LocalT*);
        total += segment_weights.capacity() * sizeof(int);
        total += segment_prefix_sums.capacity() * sizeof(int);
        total += write_buffer.capacity() * sizeof(BufferEntry);
        for (const auto& entry : write_buffer) {
            total += entry.key.capacity();
        }
        // Rough hash-map footprint: buckets + string key storage.
        total += live_weights_.bucket_count() * sizeof(void*);
        for (const auto& kv : live_weights_) {
            total += sizeof(kv) + kv.first.capacity();
        }
        for (auto* ptr : local_deltas) if (ptr) total += ptr->memory_bytes();
        return total;
    }

    bool contains_inserted(const std::string& key, int /*seg_idx*/) const {
        return weight_of(key) > 0;
    }

    bool contains_deleted(const std::string& key, int /*seg_idx*/) const {
        return weight_of(key) < 0;
    }

    bool mark_inserted(const std::string& key, int seg_idx, double start = 0.0, double end = 1.0) {
        if (seg_idx < 0 || seg_idx >= (int)P) return false;
        if (!local_deltas[seg_idx]) {
            local_deltas[seg_idx] = LocalDeltaFactory<LocalT>::create(start, end);
        }
        int committed_w = 0;
        if (local_deltas[seg_idx]) {
            committed_w = local_deltas[seg_idx]->contains_inserted(key) ? 1 :
                          (local_deltas[seg_idx]->contains_deleted(key) ? -1 : 0);
        }
        auto it = std::lower_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        bool found = (it != write_buffer.end() && it->key == key);
        int current_w = found ? it->weight : committed_w;

        if (current_w == 1) return false;

        int target_buf_w = (committed_w == 1) ? 0 : 1;
        if (target_buf_w == 0) {
            if (found) {
                write_buffer.erase(it);
            }
        } else {
            if (found) {
                it->weight = target_buf_w;
            } else {
                write_buffer.insert(it, BufferEntry{key, target_buf_w, seg_idx});
            }
        }
        // Effective live weight after insert is always +1.
        set_live_weight(key, 1);
        if (write_buffer.size() >= BUFFER_CAPACITY) {
            flush();
        }
        return true;
    }

    bool mark_deleted(const std::string& key, int seg_idx, double start = 0.0, double end = 1.0) {
        if (seg_idx < 0 || seg_idx >= (int)P) return false;
        if (!local_deltas[seg_idx]) {
            local_deltas[seg_idx] = LocalDeltaFactory<LocalT>::create(start, end);
        }
        int committed_w = 0;
        if (local_deltas[seg_idx]) {
            committed_w = local_deltas[seg_idx]->contains_inserted(key) ? 1 :
                          (local_deltas[seg_idx]->contains_deleted(key) ? -1 : 0);
        }
        auto it = std::lower_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        bool found = (it != write_buffer.end() && it->key == key);
        int current_w = found ? it->weight : committed_w;

        if (current_w == -1) return false;

        int target_buf_w = (committed_w == -1) ? 0 : -1;
        if (target_buf_w == 0) {
            if (found) {
                write_buffer.erase(it);
            }
        } else {
            if (found) {
                it->weight = target_buf_w;
            } else {
                write_buffer.insert(it, BufferEntry{key, target_buf_w, seg_idx});
            }
        }
        set_live_weight(key, -1);
        if (write_buffer.size() >= BUFFER_CAPACITY) {
            flush();
        }
        return true;
    }

    bool discard_inserted(const std::string& key, int seg_idx) {
        if (seg_idx < 0 || seg_idx >= (int)P) return false;
        int committed_w = 0;
        if (local_deltas[seg_idx]) {
            committed_w = local_deltas[seg_idx]->contains_inserted(key) ? 1 :
                          (local_deltas[seg_idx]->contains_deleted(key) ? -1 : 0);
        }
        auto it = std::lower_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        bool found = (it != write_buffer.end() && it->key == key);
        int current_w = found ? it->weight : committed_w;

        if (current_w != 1) return false;

        int target_buf_w = (committed_w == 1) ? -1 : 0;
        if (target_buf_w == 0) {
            if (found) {
                write_buffer.erase(it);
            }
        } else {
            if (found) {
                it->weight = target_buf_w;
            } else {
                write_buffer.insert(it, BufferEntry{key, target_buf_w, seg_idx});
            }
        }
        // Annihilate insert: effective weight becomes 0.
        set_live_weight(key, 0);
        if (write_buffer.size() >= BUFFER_CAPACITY) {
            flush();
        }
        return true;
    }

    bool discard_deleted(const std::string& key, int seg_idx) {
        if (seg_idx < 0 || seg_idx >= (int)P) return false;
        int committed_w = 0;
        if (local_deltas[seg_idx]) {
            committed_w = local_deltas[seg_idx]->contains_inserted(key) ? 1 :
                          (local_deltas[seg_idx]->contains_deleted(key) ? -1 : 0);
        }
        auto it = std::lower_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        bool found = (it != write_buffer.end() && it->key == key);
        int current_w = found ? it->weight : committed_w;

        if (current_w != -1) return false;

        int target_buf_w = (committed_w == -1) ? 1 : 0;
        if (target_buf_w == 0) {
            if (found) {
                write_buffer.erase(it);
            }
        } else {
            if (found) {
                it->weight = target_buf_w;
            } else {
                write_buffer.insert(it, BufferEntry{key, target_buf_w, seg_idx});
            }
        }
        // Restore deleted base key: effective weight becomes 0 (live again as base).
        set_live_weight(key, 0);
        if (write_buffer.size() >= BUFFER_CAPACITY) {
            flush();
        }
        return true;
    }

    int prefix_delta(const std::string& key, int seg_idx) const {
        if (seg_idx < 0 || seg_idx >= (int)P) return 0;
        int sum = segment_prefix_sums[seg_idx];
        if (local_deltas[seg_idx]) sum += local_deltas[seg_idx]->prefix_delta(key);
        
        auto end_it = std::lower_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        for (auto it = write_buffer.begin(); it != end_it; ++it) {
            sum += it->weight;
        }
        return sum;
    }

    int prefix_delta_le(const std::string& key, int seg_idx) const {
        if (seg_idx < 0 || seg_idx >= (int)P) return 0;
        int sum = segment_prefix_sums[seg_idx];
        if (local_deltas[seg_idx]) sum += local_deltas[seg_idx]->prefix_delta_le(key);
        
        auto end_it = std::upper_bound(write_buffer.begin(), write_buffer.end(), BufferEntry{key, 0, 0});
        for (auto it = write_buffer.begin(); it != end_it; ++it) {
            sum += it->weight;
        }
        return sum;
    }

    void iter_range(const std::string& lo, const std::string& hi, int seg_lo, int seg_hi,
                    std::vector<std::pair<std::string, int>>& out) const {
        if (seg_lo < 0) seg_lo = 0;
        if (seg_hi >= (int)P) seg_hi = (int)P - 1;
        for (int s = seg_lo; s <= seg_hi; ++s) {
            if (local_deltas[s]) local_deltas[s]->iter_range(lo, hi, out);
        }
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> out;
        for (auto* ptr : local_deltas) if (ptr) {
            auto l = ptr->inserted_list();
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> out;
        for (auto* ptr : local_deltas) if (ptr) {
            auto l = ptr->deleted_list();
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
};

#ifdef HRTLI_VERIFY_ORACLE
class RankTransportOracle {
private:
    std::map<std::string, int> keys; // key -> state (1: present, -1: deleted)

public:
    RankTransportOracle() {}

    explicit RankTransportOracle(const std::vector<std::string>& base) {
        set_base(base);
    }

    void set_base(const std::vector<std::string>& base) {
        keys.clear();
        for (const auto& k : base) {
            keys[k] = 1;
        }
    }

    void insert(const std::string& key) {
        keys[key] = 1;
    }

    void remove(const std::string& key) {
        keys[key] = -1;
    }

    bool contains(const std::string& key) const {
        auto it = keys.find(key);
        return it != keys.end() && it->second == 1;
    }

    int exact_rank(const std::string& key) const {
        if (!contains(key)) {
            throw std::runtime_error("Key not found in Oracle: " + key);
        }
        int rank = 0;
        for (const auto& pair : keys) {
            if (pair.second == 1) {
                if (pair.first < key) {
                    rank++;
                } else {
                    break;
                }
            }
        }
        return rank;
    }

    int lookup(const std::string& key) const {
        if (!contains(key)) {
            return -1;
        }
        return exact_rank(key);
    }

    int count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        int count = 0;
        for (const auto& pair : keys) {
            if (pair.second == 1) {
                if (pair.first >= lo && pair.first <= hi) {
                    count++;
                }
            }
        }
        return count;
    }

    std::vector<std::string> scan_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return {};
        std::vector<std::string> res;
        for (const auto& pair : keys) {
            if (pair.second == 1) {
                if (pair.first >= lo && pair.first <= hi) {
                    res.push_back(pair.first);
                }
            }
        }
        return res;
    }
};
#endif

class RankTransportIndex {
private:
    std::vector<std::string> base_keys;
    PiecewiseLinearModel model;
    HPSFCTable hpsfc;
    int base_epsilon;
    int consolidation_count_ = 0;

    mutable SegmentedDelta<DeltaLayerType> delta;

    // Concurrency components
    mutable std::shared_mutex rw_lock;
    bool rebuilding = false;
    std::thread rebuild_thread;
    DeltaLayerType pending_delta;
    std::unordered_set<std::string> rebuild_base_set;

#ifdef HRTLI_VERIFY_ORACLE
    mutable RankTransportOracle oracle;
#endif

    // Helper assuming lock is already held
    std::vector<std::string> snapshot_keys_under_lock() const {
        delta.flush();
        std::unordered_set<std::string> deleted;
        for (const auto& key : delta.deleted_list()) {
            deleted.insert(key);
        }

        std::vector<std::string> merged;
        merged.reserve(base_keys.size() - delta.deleted_size() + delta.inserted_size());
        for (const auto& key : base_keys) {
            if (deleted.find(key) == deleted.end()) {
                merged.push_back(key);
            }
        }
        auto inserted_keys = delta.inserted_list();
        merged.insert(merged.end(), inserted_keys.begin(), inserted_keys.end());
        std::sort(merged.begin(), merged.end());
        merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
        return merged;
    }

    bool contains_base_under_lock(const std::string& key) const {
        auto it = std::lower_bound(base_keys.begin(), base_keys.end(), key);
        return it != base_keys.end() && *it == key;
    }

    void record_pending_insert_under_lock(const std::string& key) {
        if (rebuild_base_set.find(key) != rebuild_base_set.end()) {
            pending_delta.discard_deleted(key);
        } else {
            pending_delta.mark_inserted(key);
        }
    }

    void record_pending_delete_under_lock(const std::string& key) {
        if (rebuild_base_set.find(key) != rebuild_base_set.end()) {
            pending_delta.mark_deleted(key);
        } else {
            pending_delta.discard_inserted(key);
        }
    }

public:
    RankTransportIndex(std::vector<std::string> initial_keys, int eps,
                       bool presorted_unique = false)
        : base_keys(std::move(initial_keys)), base_epsilon(eps) {
        if (!presorted_unique) {
            std::sort(base_keys.begin(), base_keys.end());
            base_keys.erase(std::unique(base_keys.begin(), base_keys.end()), base_keys.end());
        }
        model.build(base_keys, eps);
        hpsfc.build(base_keys);
        delta.init(model.segments.size());
#ifdef HRTLI_VERIFY_ORACLE
        oracle = RankTransportOracle(base_keys);
#endif
    }

    ~RankTransportIndex() {
        wait_rebuild();
    }

    void wait_rebuild() {
        if (rebuild_thread.joinable()) {
            rebuild_thread.join();
        }
    }

    size_t size() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return base_keys.size() - delta.deleted_size() + delta.inserted_size();
    }

    size_t base_size() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return base_keys.size();
    }

    size_t mutation_count() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return delta.mutation_count();
    }

    double mutation_ratio() const {
        HRTLI_LOCK_SHARED(rw_lock);
        if (base_keys.empty()) {
            return delta.mutation_count() == 0 ? 0.0 : std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(delta.mutation_count()) / static_cast<double>(base_keys.size());
    }

    int consolidation_count() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return consolidation_count_;
    }

    bool contains_base(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        return contains_base_under_lock(key);
    }

    bool contains(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        // Prefer O(1) membership hash; fall back to base fingerprint table.
        int w = delta.weight_of(key);
        if (w > 0) return true;
        if (w < 0) return false;
        return hpsfc.lookup(key, base_keys) >= 0;
    }

    bool insert(const std::string& key) {
        HRTLI_LOCK_UNIQUE(rw_lock);
        bool res = false;
        int seg_idx = model.segment_of(key);
        double start = model.segments[seg_idx].start_key_val;
        double end = model.segments[seg_idx].end_key_val;
        if (contains_base_under_lock(key)) {
            res = delta.discard_deleted(key, seg_idx);
            if (res && rebuilding) {
                record_pending_insert_under_lock(key);
            }
        } else {
            res = delta.mark_inserted(key, seg_idx, start, end);
            if (res && rebuilding) {
                record_pending_insert_under_lock(key);
            }
        }
#ifdef HRTLI_VERIFY_ORACLE
        if (res) {
            oracle.insert(key);
        }
#endif
        return res;
    }

    bool remove(const std::string& key) {
        HRTLI_LOCK_UNIQUE(rw_lock);
        bool res = false;
        int seg_idx = model.segment_of(key);
        double start = model.segments[seg_idx].start_key_val;
        double end = model.segments[seg_idx].end_key_val;
        if (delta.discard_inserted(key, seg_idx)) {
            res = true;
        } else if (contains_base_under_lock(key)) {
            if (!delta.contains_deleted(key, seg_idx)) {
                res = delta.mark_deleted(key, seg_idx, start, end);
            }
        }
        if (res && rebuilding) {
            record_pending_delete_under_lock(key);
        }
#ifdef HRTLI_VERIFY_ORACLE
        if (res) {
            oracle.remove(key);
        }
#endif
        return res;
    }

    int delta_before(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        int seg_idx = model.segment_of(key);
        return delta.prefix_delta(key, seg_idx);
    }

    int transported_predict(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        int base_idx = hpsfc.lookup(key, base_keys);
        if (base_idx < 0) {
            throw std::runtime_error("Transported prediction is defined only for base keys: " + key);
        }

        int seg_idx = model.segment_of(key);
        if (delta.contains_deleted(key, seg_idx)) {
            throw std::runtime_error("Transported prediction is undefined for deleted base key: " + key);
        }

        int prediction = model.predict(hli_key_to_double(key)) + delta.prefix_delta(key, seg_idx);
#ifdef HRTLI_VERIFY_ORACLE
        assert(std::abs(prediction - oracle.exact_rank(key)) <= base_epsilon);
#endif
        return prediction;
    }

    int certified_epsilon() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return base_epsilon;
    }

    int exact_rank(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        auto it = std::lower_bound(base_keys.begin(), base_keys.end(), key);
        int seg_idx = model.segment_of(key);
        if (delta.contains_inserted(key, seg_idx)) {
            int base_less = std::distance(base_keys.begin(), it);
            int res = base_less + delta.prefix_delta(key, seg_idx);
#ifdef HRTLI_VERIFY_ORACLE
            assert(res == oracle.exact_rank(key));
#endif
            return res;
        }
        if (it != base_keys.end() && *it == key && !delta.contains_deleted(key, seg_idx)) {
            int base_less = std::distance(base_keys.begin(), it);
            int res = base_less + delta.prefix_delta(key, seg_idx);
#ifdef HRTLI_VERIFY_ORACLE
            assert(res == oracle.exact_rank(key));
#endif
            return res;
        }
#ifdef HRTLI_VERIFY_ORACLE
        assert(!oracle.contains(key));
#endif
        throw std::runtime_error("Key not found in RankTransportIndex: " + key);
    }

    int lookup(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);

        // Zero-delta fast path: pure HP-SFC, no segment_of / prefix_delta.
        if (delta.empty()) {
            int idx = hpsfc.lookup(key, base_keys);
#ifdef HRTLI_VERIFY_ORACLE
            assert(idx == oracle.lookup(key));
#endif
            return idx;  // -1 if not found
        }

        // O(1) membership branch before paying segment_of + prefix sum.
        int w = delta.weight_of(key);
        if (w > 0) {
            int seg_idx = model.segment_of(key);
            auto it = std::lower_bound(base_keys.begin(), base_keys.end(), key);
            int base_less = static_cast<int>(std::distance(base_keys.begin(), it));
            int res = base_less + delta.prefix_delta(key, seg_idx);
#ifdef HRTLI_VERIFY_ORACLE
            assert(res == oracle.lookup(key));
#endif
            return res;
        }
        if (w < 0) {
#ifdef HRTLI_VERIFY_ORACLE
            assert(-1 == oracle.lookup(key));
#endif
            return -1;
        }

        // Base-key path with a non-empty delta.
        int idx = hpsfc.lookup(key, base_keys);
        if (idx >= 0) {
            int seg_idx = model.segment_of(key);
            int res = idx + delta.prefix_delta(key, seg_idx);
#ifdef HRTLI_VERIFY_ORACLE
            assert(res == oracle.lookup(key));
#endif
            return res;
        }

#ifdef HRTLI_VERIFY_ORACLE
        assert(-1 == oracle.lookup(key));
#endif
        return -1;
    }

    bool point_lookup(const std::string& key) const {
        HRTLI_LOCK_SHARED(rw_lock);
        // Hot path for paper point-lookup tables:
        // 1) empty delta -> pure HP-SFC expected O(1)
        // 2) else O(1) mutation membership, then HP-SFC for base keys
        // No segment_of, no B-tree walk on membership.
        if (delta.empty()) {
            bool res = hpsfc.lookup(key, base_keys) >= 0;
#ifdef HRTLI_VERIFY_ORACLE
            assert(res == oracle.contains(key));
#endif
            return res;
        }
        int w = delta.weight_of(key);
        if (w > 0) {
#ifdef HRTLI_VERIFY_ORACLE
            assert(true == oracle.contains(key));
#endif
            return true;
        }
        if (w < 0) {
#ifdef HRTLI_VERIFY_ORACLE
            assert(false == oracle.contains(key));
#endif
            return false;
        }
        bool res = hpsfc.lookup(key, base_keys) >= 0;
#ifdef HRTLI_VERIFY_ORACLE
        assert(res == oracle.contains(key));
#endif
        return res;
    }

    int count_range(const std::string& lo, const std::string& hi) const {
        HRTLI_LOCK_SHARED(rw_lock);
        if (lo > hi) {
#ifdef HRTLI_VERIFY_ORACLE
            assert(0 == oracle.count_range(lo, hi));
#endif
            return 0;
        }
        // rank_at_or_after(lo): number of live keys strictly less than lo
        int base_less_lo = static_cast<int>(
            std::lower_bound(base_keys.begin(), base_keys.end(), lo) - base_keys.begin());
        int seg_lo = model.segment_of(lo);
        int rk_lo = base_less_lo + delta.prefix_delta(lo, seg_lo);
        // rank_after(hi): number of live keys less than or equal to hi
        int base_leq_hi = static_cast<int>(
            std::upper_bound(base_keys.begin(), base_keys.end(), hi) - base_keys.begin());
        int seg_hi = model.segment_of(hi);
        int rk_hi = base_leq_hi + delta.prefix_delta_le(hi, seg_hi);
        int res = rk_hi - rk_lo;
#ifdef HRTLI_VERIFY_ORACLE
        assert(res == oracle.count_range(lo, hi));
#endif
        return res;
    }

    std::vector<std::string> scan_range(const std::string& lo, const std::string& hi) const {
        HRTLI_LOCK_SHARED(rw_lock);
        if (lo > hi) {
#ifdef HRTLI_VERIFY_ORACLE
            assert(oracle.scan_range(lo, hi).empty());
#endif
            return {};
        }

        // Base-key cursor
        auto base_lo = std::lower_bound(base_keys.begin(), base_keys.end(), lo);
        auto base_hi = std::upper_bound(base_keys.begin(), base_keys.end(), hi);

        // Perform the O(B) prefilter pass to collect all buffer entries in [lo, hi].
        std::vector<SegmentedDelta<DeltaLayerType>::BufferEntry> buf_entries;
        auto buf_start = std::lower_bound(delta.get_write_buffer().begin(), delta.get_write_buffer().end(), SegmentedDelta<DeltaLayerType>::BufferEntry{lo, 0, 0});
        auto buf_end = std::upper_bound(delta.get_write_buffer().begin(), delta.get_write_buffer().end(), SegmentedDelta<DeltaLayerType>::BufferEntry{hi, 0, 0});
        for (auto it = buf_start; it != buf_end; ++it) {
            buf_entries.push_back(*it);
        }

        // Delta cursor: bounded in-order traversal.
        std::vector<std::pair<std::string, int>> delta_entries;
        int seg_lo = model.segment_of(lo);
        int seg_hi = model.segment_of(hi);
        delta.iter_range(lo, hi, seg_lo, seg_hi, delta_entries);

        if (buf_entries.empty()) {
            std::vector<std::string> result;
            auto base_it = base_lo;
            size_t delta_pos = 0;

            while (base_it != base_hi || delta_pos < delta_entries.size()) {
                const std::string* bk = (base_it != base_hi) ? &(*base_it) : nullptr;
                const std::string* dk = (delta_pos < delta_entries.size()) ? &delta_entries[delta_pos].first : nullptr;
                int dw = (delta_pos < delta_entries.size()) ? delta_entries[delta_pos].second : 0;

                if (bk && (!dk || *bk < *dk)) {
                    result.push_back(*bk);
                    ++base_it;
                } else if (dk && (!bk || *dk < *bk)) {
                    if (dw > 0) {
                        result.push_back(*dk);
                    }
                    ++delta_pos;
                } else if (bk && dk && *bk == *dk) {
                    if (dw > 0) {
                        result.push_back(*bk);
                    }
                    ++base_it;
                    ++delta_pos;
                } else {
                    break;
                }
            }
#ifdef HRTLI_VERIFY_ORACLE
            assert(result == oracle.scan_range(lo, hi));
#endif
            return result;
        } else {
            std::vector<std::string> result;
            auto base_it = base_lo;
            size_t delta_pos = 0;
            size_t buf_pos = 0;

            while (base_it != base_hi || delta_pos < delta_entries.size() || buf_pos < buf_entries.size()) {
                const std::string* bk = (base_it != base_hi) ? &(*base_it) : nullptr;
                const std::string* dk = (delta_pos < delta_entries.size()) ? &delta_entries[delta_pos].first : nullptr;
                const std::string* wk = (buf_pos < buf_entries.size()) ? &buf_entries[buf_pos].key : nullptr;

                const std::string* min_key_ptr = nullptr;
                if (bk) min_key_ptr = bk;
                if (dk && (!min_key_ptr || *dk < *min_key_ptr)) min_key_ptr = dk;
                if (wk && (!min_key_ptr || *wk < *min_key_ptr)) min_key_ptr = wk;

                if (!min_key_ptr) break;
                const std::string& min_key = *min_key_ptr;

                bool base_match = (bk && *bk == min_key);
                bool delta_match = (dk && *dk == min_key);
                bool buf_match = (wk && *wk == min_key);

                if (buf_match) {
                    int ww = buf_entries[buf_pos].weight;
                    if (ww > 0) {
                        result.push_back(min_key);
                    }
                } else if (delta_match) {
                    int dw = delta_entries[delta_pos].second;
                    if (dw > 0) {
                        result.push_back(min_key);
                    }
                } else if (base_match) {
                    result.push_back(min_key);
                }

                if (base_match) ++base_it;
                if (delta_match) ++delta_pos;
                if (buf_match) ++buf_pos;
            }
#ifdef HRTLI_VERIFY_ORACLE
            assert(result == oracle.scan_range(lo, hi));
#endif
            return result;
        }
    }

    std::vector<std::string> snapshot_keys() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return snapshot_keys_under_lock();
    }

    size_t consolidate() {
        HRTLI_LOCK_UNIQUE(rw_lock);
        if (rebuilding) {
            return 0; 
        }
        delta.flush();
        size_t mutations = delta.mutation_count();
        if (mutations == 0) {
            return 0;
        }

        rebuilding = true;
        auto snapshot = snapshot_keys_under_lock();
        rebuild_base_set.clear();
        for (const auto& key : snapshot) {
            rebuild_base_set.insert(key);
        }

        if (rebuild_thread.joinable()) {
            rebuild_thread.join();
        }

        pending_delta.reset();

        rebuild_thread = std::thread([this, snapshot]() {
            PiecewiseLinearModel new_model;
            new_model.build(snapshot, base_epsilon);
            HPSFCTable new_hpsfc;
            new_hpsfc.build(snapshot);

            {
                HRTLI_LOCK_UNIQUE(this->rw_lock);
                this->base_keys = std::move(snapshot);
#ifdef HRTLI_VERIFY_ORACLE
                this->oracle.set_base(this->base_keys);
#endif
                this->model = std::move(new_model);
                this->hpsfc = std::move(new_hpsfc);
                
                SegmentedDelta<DeltaLayerType> new_segmented_delta(this->model.segments.size());
                for (const auto& key : this->pending_delta.inserted_list()) {
                    int seg_idx = this->model.segment_of(key);
                    double start = this->model.segments[seg_idx].start_key_val;
                    double end = this->model.segments[seg_idx].end_key_val;
                    new_segmented_delta.mark_inserted(key, seg_idx, start, end);
                }
                for (const auto& key : this->pending_delta.deleted_list()) {
                    int seg_idx = this->model.segment_of(key);
                    double start = this->model.segments[seg_idx].start_key_val;
                    double end = this->model.segments[seg_idx].end_key_val;
                    new_segmented_delta.mark_deleted(key, seg_idx, start, end);
                }
                this->delta = std::move(new_segmented_delta);
                this->pending_delta.reset();

                this->rebuild_base_set.clear();
                this->rebuilding = false;
                ++(this->consolidation_count_);
            }
        });

        return mutations;
    }

    bool should_consolidate(double threshold) const {
        HRTLI_LOCK_SHARED(rw_lock);
        if (threshold <= 0.0) {
            throw std::invalid_argument("consolidation threshold must be positive");
        }
        size_t mutations = delta.mutation_count();
        double ratio = base_keys.empty() ? (mutations == 0 ? 0.0 : std::numeric_limits<double>::infinity())
                                         : static_cast<double>(mutations) / static_cast<double>(base_keys.size());
        return mutations > 0 && ratio >= threshold;
    }

    bool maybe_consolidate(double threshold) {
        HRTLI_LOCK_UNIQUE(rw_lock);
        if (rebuilding) {
            return false;
        }
        delta.flush();
        size_t mutations = delta.mutation_count();
        double ratio = base_keys.empty() ? (mutations == 0 ? 0.0 : std::numeric_limits<double>::infinity())
                                         : static_cast<double>(mutations) / static_cast<double>(base_keys.size());
        if (mutations > 0 && ratio >= threshold) {
            rebuilding = true;
            auto snapshot = snapshot_keys_under_lock();
            rebuild_base_set.clear();
            for (const auto& key : snapshot) {
                rebuild_base_set.insert(key);
            }

            if (rebuild_thread.joinable()) {
                rebuild_thread.join();
            }

            pending_delta.reset();

            rebuild_thread = std::thread([this, snapshot]() {
                PiecewiseLinearModel new_model;
                new_model.build(snapshot, base_epsilon);
                HPSFCTable new_hpsfc;
                new_hpsfc.build(snapshot);

                {
                    HRTLI_LOCK_UNIQUE(this->rw_lock);
                    this->base_keys = std::move(snapshot);
#ifdef HRTLI_VERIFY_ORACLE
                    this->oracle.set_base(this->base_keys);
#endif
                    this->model = std::move(new_model);
                    this->hpsfc = std::move(new_hpsfc);
                    
                    SegmentedDelta<DeltaLayerType> new_segmented_delta(this->model.segments.size());
                    for (const auto& key : this->pending_delta.inserted_list()) {
                        int seg_idx = this->model.segment_of(key);
                        double start = this->model.segments[seg_idx].start_key_val;
                        double end = this->model.segments[seg_idx].end_key_val;
                        new_segmented_delta.mark_inserted(key, seg_idx, start, end);
                    }
                    for (const auto& key : this->pending_delta.deleted_list()) {
                        int seg_idx = this->model.segment_of(key);
                        double start = this->model.segments[seg_idx].start_key_val;
                        double end = this->model.segments[seg_idx].end_key_val;
                        new_segmented_delta.mark_deleted(key, seg_idx, start, end);
                    }
                    this->delta = std::move(new_segmented_delta);
                    this->pending_delta.reset();

                    this->rebuild_base_set.clear();
                    this->rebuilding = false;
                    ++(this->consolidation_count_);
                }
            });
            return true;
        }
        return false;
    }

    void get_memory_breakdown(size_t &base_keys_sz, size_t &model_sz, size_t &hpsfc_sz, size_t &delta_sz) const {
        HRTLI_LOCK_UNIQUE(rw_lock);
        delta.flush();
        base_keys_sz = base_keys.size() * sizeof(std::string);
        for (const auto& k : base_keys) base_keys_sz += k.capacity();
        model_sz = model.segments.size() * sizeof(PiecewiseLinearModel::Segment);
        hpsfc_sz = hpsfc.table_size() * sizeof(HPSFCTable::Slot);
        delta_sz = delta.memory_bytes();
        if (rebuilding) {
            delta_sz += pending_delta.memory_bytes();
        }
    }

    size_t model_segments_count() const {
        HRTLI_LOCK_SHARED(rw_lock);
        return model.segments.size();
    }

    int get_model_max_error() const {
        HRTLI_LOCK_SHARED(rw_lock);
        int max_err = 0;
        for (int i = 0; i < (int)base_keys.size(); ++i) {
            double val = hli_key_to_double(base_keys[i]);
            int pred = model.predict(val);
            int err = std::abs(pred - i);
            if (err > max_err) {
                max_err = err;
            }
        }
        return max_err;
    }

    size_t memory_bytes() const {
        HRTLI_LOCK_SHARED(rw_lock);
        size_t mem = 0;
        mem += base_keys.size() * sizeof(std::string);
        for (const auto& k : base_keys) mem += k.capacity();
        mem += model.segments.size() * sizeof(PiecewiseLinearModel::Segment);
        mem += hpsfc.table_size() * sizeof(HPSFCTable::Slot);
        mem += delta.memory_bytes();
        if (rebuilding) {
            mem += pending_delta.memory_bytes();
        }
        return mem;
    }
};

} // namespace hrtli
