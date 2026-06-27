/**
 * SortedFenwickDelta — cache-friendly replacement for SignedDeltaTreap.
 *
 * Instead of a pointer-chasing BST where each node is a separately
 * heap-allocated object (O(m) random cache misses per prefix query), this
 * structure stores mutations in a sorted vector<pair<string,int>> (compact,
 * cache-friendly) and layers a Fenwick tree (BIT) over the weights so that
 * prefix-sum queries walk a flat integer array rather than scattered pointers.
 *
 * Complexity (m = number of live mutations):
 *   prefix_delta : O(log m) with excellent cache behaviour (BIT index walk)
 *   insert/update: O(m) amortized rebuild when the sorted array must resize,
 *                  O(log m) BIT update when key already exists at known index
 *   For the read-dominated workloads targeted by HRT-LI the fast prefix-query
 *   path is the critical one.
 *
 * For small m (< SMALL_THRESHOLD) we use a plain sorted linear scan which
 * fits in a single cache line and beats the BIT overhead entirely.
 */

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace hrtli {

static constexpr int SMALL_DELTA_THRESHOLD = 32;  // linear scan below this

class SortedFenwickDelta {
public:
    // ---- Public interface mirrors SignedDeltaTreap -------------------------

    SortedFenwickDelta() = default;

    // Move-only (same contract as treap)
    SortedFenwickDelta(const SortedFenwickDelta&) = delete;
    SortedFenwickDelta& operator=(const SortedFenwickDelta&) = delete;
    SortedFenwickDelta(SortedFenwickDelta&&) noexcept = default;
    SortedFenwickDelta& operator=(SortedFenwickDelta&&) noexcept = default;

    void reset() {
        keys_.clear();
        weights_.clear();
        bit_.clear();
        inserted_count_ = 0;
        deleted_count_  = 0;
        dirty_           = false;
    }

    int size()           const { return static_cast<int>(keys_.size()); }
    int inserted_size()  const { return inserted_count_; }
    int deleted_size()   const { return deleted_count_; }
    int mutation_count() const { return size(); }
    bool empty()         const { return keys_.empty(); }

    // --- Mutation registration ---------------------------------------------

    bool mark_inserted(const std::string& key) {
        return set_weight(key, +1);
    }

    bool mark_deleted(const std::string& key) {
        return set_weight(key, -1);
    }

    bool discard_inserted(const std::string& key) {
        return discard_weight(key, +1);
    }

    bool discard_deleted(const std::string& key) {
        return discard_weight(key, -1);
    }

    // --- Query interface ---------------------------------------------------

    bool contains_inserted(const std::string& key) const {
        int idx = lower_index(key);
        return idx < size() && keys_[idx] == key && weights_[idx] > 0;
    }

    bool contains_deleted(const std::string& key) const {
        int idx = lower_index(key);
        return idx < size() && keys_[idx] == key && weights_[idx] < 0;
    }

    /**
     * prefix_delta(key) = sum of weights[i] for all keys_[i] < key
     * (strict less-than, mirrors the treap's semantics).
     */
    int prefix_delta(const std::string& key) const {
        // Fast path: empty delta
        if (keys_.empty()) return 0;

        // Position: number of keys strictly less than `key`
        int pos = lower_index(key);   // index of first key >= key
        if (pos == 0) return 0;

        // Small-delta fast path: linear accumulate, single cache line
        if (size() <= SMALL_DELTA_THRESHOLD) {
            int total = 0;
            for (int i = 0; i < pos; ++i) total += weights_[i];
            return total;
        }

        // BIT path
        ensure_bit_valid();
        return bit_prefix(pos);  // sum of bit[1..pos]
    }

    /**
     * prefix_delta_le(key) = sum of weights[i] for all keys_[i] <= key
     */
    int prefix_delta_le(const std::string& key) const {
        if (keys_.empty()) return 0;

        // upper_bound position: first key strictly > key
        int pos = upper_index(key);
        if (pos == 0) return 0;

        if (size() <= SMALL_DELTA_THRESHOLD) {
            int total = 0;
            for (int i = 0; i < pos; ++i) total += weights_[i];
            return total;
        }

        ensure_bit_valid();
        return bit_prefix(pos);
    }

    /** Collect (key, weight) pairs in [lo, hi] in sorted order. */
    void iter_range(const std::string& lo, const std::string& hi,
                    std::vector<std::pair<std::string, int>>& out) const {
        if (keys_.empty()) return;
        int start = lower_index(lo);
        int end   = upper_index(hi);
        out.reserve(out.size() + static_cast<size_t>(end - start));
        for (int i = start; i < end; ++i) {
            out.emplace_back(keys_[i], weights_[i]);
        }
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> out;
        for (int i = 0; i < size(); ++i) {
            if (weights_[i] > 0) out.push_back(keys_[i]);
        }
        return out;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> out;
        for (int i = 0; i < size(); ++i) {
            if (weights_[i] < 0) out.push_back(keys_[i]);
        }
        return out;
    }

private:
    // ---- Storage ----------------------------------------------------------
    // keys_ and weights_ are kept in parallel, sorted by keys_.
    std::vector<std::string> keys_;
    std::vector<int>         weights_;   // +1 inserted, -1 deleted

    // Fenwick tree (1-indexed BIT) over weights_.
    // bit_[i] covers a range of weights_ determined by the lowest set bit.
    mutable std::vector<int> bit_;
    mutable bool             dirty_ = false;  // BIT needs rebuild

    int inserted_count_ = 0;
    int deleted_count_  = 0;

    // ---- Index helpers ----------------------------------------------------

    int lower_index(const std::string& key) const {
        return static_cast<int>(
            std::lower_bound(keys_.begin(), keys_.end(), key) - keys_.begin());
    }

    int upper_index(const std::string& key) const {
        return static_cast<int>(
            std::upper_bound(keys_.begin(), keys_.end(), key) - keys_.begin());
    }

    // ---- Mutation helpers -------------------------------------------------

    bool set_weight(const std::string& key, int w) {
        int idx = lower_index(key);
        if (idx < size() && keys_[idx] == key) {
            if (weights_[idx] == w) return false;  // no change
            // Update counters
            if (weights_[idx] > 0) --inserted_count_;
            else if (weights_[idx] < 0) --deleted_count_;
            weights_[idx] = w;
            if (w > 0) ++inserted_count_;
            else if (w < 0) ++deleted_count_;
            dirty_ = true;
            return true;
        }
        // Insert at position idx, maintaining sort order
        keys_.insert(keys_.begin() + idx, key);
        weights_.insert(weights_.begin() + idx, w);
        if (w > 0) ++inserted_count_;
        else if (w < 0) ++deleted_count_;
        dirty_ = true;
        return true;
    }

    bool discard_weight(const std::string& key, int w) {
        int idx = lower_index(key);
        if (idx >= size() || keys_[idx] != key || weights_[idx] != w) {
            return false;
        }
        if (w > 0) --inserted_count_;
        else if (w < 0) --deleted_count_;
        keys_.erase(keys_.begin() + idx);
        weights_.erase(weights_.begin() + idx);
        dirty_ = true;
        return true;
    }

    // ---- Fenwick tree helpers ---------------------------------------------

    void ensure_bit_valid() const {
        if (!dirty_ && bit_.size() == static_cast<size_t>(size() + 1)) return;
        // Full rebuild: O(m) — amortized over queries
        int m = size();
        bit_.assign(m + 1, 0);
        for (int i = 0; i < m; ++i) {
            bit_update(i + 1, weights_[i]);
        }
        dirty_ = false;
    }

    void bit_update(int i, int delta) const {
        int m = size();
        for (; i <= m; i += i & (-i)) {
            bit_[i] += delta;
        }
    }

    // Prefix sum [1..i] (1-indexed)
    int bit_prefix(int i) const {
        int s = 0;
        for (; i > 0; i -= i & (-i)) {
            s += bit_[i];
        }
        return s;
    }
};

}  // namespace hrtli
