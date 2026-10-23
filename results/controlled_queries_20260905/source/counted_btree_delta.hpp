/**
 * counted_btree_delta.hpp — Cache-friendly Counted B+-tree delta layer.
 *
 * MOTIVATION
 * ----------
 * A pointer-chasing treap allocates each node separately, scattering nodes
 * across memory. Every prefix_delta() traversal can therefore pay many cache
 * misses.
 *
 * This B+-tree packs 2*B keys and weights per node in a flat array, so a
 * single cache line fetch covers up to B=16 keys. Traversal at B=16 touches
 * ceil(log_16(m)) approx ceil(log(m)/4) nodes instead of the binary-tree depth.
 *
 * Leaf and internal nodes are ordered by the full string key. A previous
 * prototype tried to route internal nodes by 64-bit fingerprints, but rank
 * prefix sums require the tree order to match lexicographic key order exactly.
 * The corrected implementation keeps no fingerprint routing metadata.
 *
 * INTERFACE
 * ---------
 * Mirrors SignedDeltaTreap exactly so rank_transport.hpp can swap it in
 * by changing one typedef. All three structures (treap, Fenwick, B+-tree)
 * share the same public API.
 *
 * CORRECTNESS
 * -----------
 * prefix_delta(k) computes sum w[i] for all stored keys[i] < k (strict).
 * prefix_delta_le(k) computes sum w[i] for all stored keys[i] <= k.
 * These are exact — no approximation. Theorem 2 (epsilon-preservation) holds.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <string>
#include <vector>

namespace hrtli {

// B = branching factor / keys per leaf node.
// B=16 fills ~1 cache line per node for short-string pointer+weight pairs.
// Increase to 32 for workloads with very large m.
static constexpr int BTREE_B = 16;

class CountedBTreeDelta {
public:
    CountedBTreeDelta() { root_ = new Node(/*leaf=*/true); }

    ~CountedBTreeDelta() { destroy(root_); }

    // Move-only
    CountedBTreeDelta(const CountedBTreeDelta&) = delete;
    CountedBTreeDelta& operator=(const CountedBTreeDelta&) = delete;

    CountedBTreeDelta(CountedBTreeDelta&& o) noexcept
        : root_(o.root_), size_(o.size_),
          inserted_count_(o.inserted_count_), deleted_count_(o.deleted_count_) {
        o.root_ = new Node(true); o.size_ = 0;
        o.inserted_count_ = 0; o.deleted_count_ = 0;
    }

    CountedBTreeDelta& operator=(CountedBTreeDelta&& o) noexcept {
        if (this != &o) {
            destroy(root_);
            root_ = o.root_; size_ = o.size_;
            inserted_count_ = o.inserted_count_;
            deleted_count_  = o.deleted_count_;
            o.root_ = new Node(true); o.size_ = 0;
            o.inserted_count_ = 0; o.deleted_count_ = 0;
        }
        return *this;
    }

    void reset() {
        destroy(root_);
        root_ = new Node(true);
        size_ = 0; inserted_count_ = 0; deleted_count_ = 0;
    }

    int size()           const { return size_; }
    int mutation_count() const { return size_; }
    int inserted_size()  const { return inserted_count_; }
    int deleted_size()   const { return deleted_count_; }
    bool empty()         const { return size_ == 0; }
    size_t memory_bytes() const { return memory_bytes_node(root_); }
    bool validate_internal_lcps() const {
        return validate_node_lcps(root_);
    }

    // Mutation registration

    bool mark_inserted(const std::string& key) { return set_weight(key, +1); }
    bool mark_deleted (const std::string& key) { return set_weight(key, -1); }
    bool discard_inserted(const std::string& key) { return discard_weight(key, +1); }
    bool discard_deleted (const std::string& key) { return discard_weight(key, -1); }

    // Queries

    bool contains_inserted(const std::string& key) const {
        return find_weight(key) == +1;
    }
    bool contains_deleted(const std::string& key) const {
        return find_weight(key) == -1;
    }

    /**
     * prefix_delta(key): sum of w[i] for all stored keys strictly < key.
     *
     * Core hot path. Walk from root to leaf, at each internal node:
     *   - add subtree_sum of all children whose pivot < key
     * At leaf:
     *   - add weights of all entries strictly < key
     * Total nodes visited = O(log_B(m)) approx 6 for m=10M with B=16.
     *
     * Internal node comparisons use full lexicographic keys so prefix sums are
     * accumulated in the same order as the public key comparator.
     */
    int prefix_delta(const std::string& key) const {
        if (size_ == 0) return 0;
        return prefix_sum_lt(root_, key);
    }

    /**
     * prefix_delta_le(key): sum of w[i] for all stored keys <= key.
     */
    int prefix_delta_le(const std::string& key) const {
        if (size_ == 0) return 0;
        return prefix_sum_le(root_, key);
    }

    /** Collect (key, weight) pairs in [lo, hi] in sorted order. */
    void iter_range(const std::string& lo, const std::string& hi,
                    std::vector<std::pair<std::string,int>>& out) const {
        collect_range(root_, lo, hi, out);
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> out;
        collect_by_weight(root_, +1, out);
        return out;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> out;
        collect_by_weight(root_, -1, out);
        return out;
    }

private:
    // Internal node structure
    //
    // Leaf node: holds up to 2*B (key, weight) pairs in sorted order.
    //   keys[]       - full std::string, sorted lexicographically.
    //   weights[]    - signed integer weights parallel to keys[].
    //
    // Internal node: holds up to 2*B lexicographic pivot strings and 2*B+1
    //   child pointers with precomputed subtree_sums.
    //
    // The is_leaf flag discriminates between the two node types.

    struct Node {
        bool is_leaf;
        int  count = 0;  // number of keys/pivots stored

        // Leaf-only: full string keys (sorted lexicographically)
        std::array<std::string, 2*BTREE_B> keys;

        // Leaf: weights parallel to keys
        std::array<int, 2*BTREE_B> weights = {};

        // Leaf: prefix sum (weights_prefix[i] = sum of weights[0..i-1])
        std::array<int, 2*BTREE_B+1> weights_prefix = {};

        // Leaf-only: LCP between keys[i] and keys[i-1], with lcps[0] = 0
        std::array<int, 2*BTREE_B> lcps = {};

        // Internal-only: full pivot strings for lexicographic routing
        std::array<std::string, 2*BTREE_B> pivot_keys;

        // Internal-only: LCP between pivot_keys[i] and pivot_keys[i-1], with pivot_lcps[0] = 0
        std::array<int, 2*BTREE_B> pivot_lcps = {};

        // Internal only: child pointers (count+1 children)
        std::array<Node*, 2*BTREE_B+1> children = {};

        // Internal only: subtree_sum[i] = sum of all weights in subtree children[i]
        std::array<int, 2*BTREE_B+1> subtree_sum = {};

        // Leaf linked list for sequential scans
        Node* next_leaf = nullptr;

        explicit Node(bool leaf) : is_leaf(leaf) {
            children.fill(nullptr);
            subtree_sum.fill(0);
            weights.fill(0);
            weights_prefix.fill(0);
            lcps.fill(0);
            pivot_lcps.fill(0);
        }
    };

    Node* root_ = nullptr;
    int   size_           = 0;
    int   inserted_count_ = 0;
    int   deleted_count_  = 0;

    // Destroy

    void destroy(Node* n) {
        if (!n) return;
        if (!n->is_leaf) {
            for (int i = 0; i <= n->count; ++i) destroy(n->children[i]);
        }
        delete n;
    }

    // Find

    int find_weight(const std::string& key) const {
        Node* n = root_;
        while (!n->is_leaf) {
            int pos = upper_bound_pos(n, key);
            n = n->children[pos];
        }
        int pos = lower_bound_pos_leaf(n, key);
        if (pos < n->count && n->keys[pos] == key) return n->weights[pos];
        return 0;
    }

    // Prefix sums

    int prefix_sum_lt(Node* n, const std::string& key) const {
        if (n->is_leaf) {
            // Sum weights of all entries strictly < key (full string compare at leaf)
            int pos = lower_bound_pos_leaf(n, key);
            return n->weights_prefix[pos];
        }
        // Internal: find child index using lexicographic pivot comparison
        int pos = upper_bound_pos(n, key);
        int total = 0;
        for (int i = 0; i < pos; ++i) total += n->subtree_sum[i];
        total += prefix_sum_lt(n->children[pos], key);
        return total;
    }

    int prefix_sum_le(Node* n, const std::string& key) const {
        if (n->is_leaf) {
            int pos = upper_bound_pos_leaf(n, key);  // first entry > key
            return n->weights_prefix[pos];
        }
        int pos = upper_bound_pos(n, key);
        int total = 0;
        for (int i = 0; i < pos; ++i) total += n->subtree_sum[i];
        total += prefix_sum_le(n->children[pos], key);
        return total;
    }

    // Search helpers

    static inline int compute_lcp(const std::string& a, const std::string& b) {
        int max_len = std::min(static_cast<int>(a.size()), static_cast<int>(b.size()));
        int lcp = 0;
        while (lcp < max_len && a[lcp] == b[lcp]) {
            ++lcp;
        }
        return lcp;
    }

    void rebuild_lcps(Node* node) {
        if (node->is_leaf) {
            node->lcps[0] = 0;
            for (int i = 1; i < node->count; ++i) {
                node->lcps[i] = compute_lcp(node->keys[i], node->keys[i-1]);
            }
        } else {
            node->pivot_lcps[0] = 0;
            for (int i = 1; i < node->count; ++i) {
                node->pivot_lcps[i] = compute_lcp(node->pivot_keys[i], node->pivot_keys[i-1]);
            }
        }
    }

    template<bool IsLeaf, bool IsLowerBound>
    int lcp_binary_search(Node* n, const std::string& key) const {
        int count = n->count;
        if (count == 0) return 0;
        
        auto get_key = [&](int idx) -> const std::string& {
            if constexpr (IsLeaf) {
                return n->keys[idx];
            } else {
                return n->pivot_keys[idx];
            }
        };
        
        auto get_lcp = [&](int idx) -> int {
            if constexpr (IsLeaf) {
                return n->lcps[idx];
            } else {
                return n->pivot_lcps[idx];
            }
        };

        int L = -1;
        int R = count;
        int l = 0;
        int r = 0;

        while (L + 1 < R) {
            int mid = (L + R) / 2;
            
            bool use_L = false;
            if (L == -1 && R == count) {
                // Both virtual, do full comparison
            } else if (L == -1) {
                use_L = false;
            } else if (R == count) {
                use_L = true;
            } else {
                use_L = (l >= r);
            }

            const std::string& s_mid = get_key(mid);

            if (use_L) {
                int h_L = get_lcp(L + 1);
                for (int i = L + 2; i <= mid; ++i) {
                    int val = get_lcp(i);
                    if (val < h_L) h_L = val;
                }

                if (l < h_L) {
                    L = mid;
                } else if (l > h_L) {
                    R = mid;
                    r = h_L;
                } else {
                    int match = l;
                    int min_len = std::min(static_cast<int>(key.size()), static_cast<int>(s_mid.size()));
                    while (match < min_len && key[match] == s_mid[match]) {
                        ++match;
                    }
                    
                    bool key_less = false;
                    if (match < min_len) {
                        key_less = (key[match] < s_mid[match]);
                    } else {
                        key_less = (static_cast<int>(key.size()) < static_cast<int>(s_mid.size()));
                    }

                    if (key_less) {
                        R = mid;
                        r = match;
                    } else {
                        bool is_equal = (match == static_cast<int>(key.size()) && match == static_cast<int>(s_mid.size()));
                        if (is_equal && IsLowerBound) {
                            R = mid;
                            r = match;
                        } else {
                            L = mid;
                            l = match;
                        }
                    }
                }
            } else if (L != -1 || R != count) {
                int h_R = get_lcp(mid + 1);
                for (int i = mid + 2; i <= R; ++i) {
                    int val = get_lcp(i);
                    if (val < h_R) h_R = val;
                }

                if (r < h_R) {
                    R = mid;
                } else if (r > h_R) {
                    L = mid;
                    l = h_R;
                } else {
                    int match = r;
                    int min_len = std::min(static_cast<int>(key.size()), static_cast<int>(s_mid.size()));
                    while (match < min_len && key[match] == s_mid[match]) {
                        ++match;
                    }

                    bool key_less = false;
                    if (match < min_len) {
                        key_less = (key[match] < s_mid[match]);
                    } else {
                        key_less = (static_cast<int>(key.size()) < static_cast<int>(s_mid.size()));
                    }

                    if (key_less) {
                        R = mid;
                        r = match;
                    } else {
                        bool is_equal = (match == static_cast<int>(key.size()) && match == static_cast<int>(s_mid.size()));
                        if (is_equal && IsLowerBound) {
                            R = mid;
                            r = match;
                        } else {
                            L = mid;
                            l = match;
                        }
                    }
                }
            } else {
                int match = 0;
                int min_len = std::min(static_cast<int>(key.size()), static_cast<int>(s_mid.size()));
                while (match < min_len && key[match] == s_mid[match]) {
                    ++match;
                }

                bool key_less = false;
                if (match < min_len) {
                    key_less = (key[match] < s_mid[match]);
                } else {
                    key_less = (static_cast<int>(key.size()) < static_cast<int>(s_mid.size()));
                }

                if (key_less) {
                    R = mid;
                    r = match;
                } else {
                    bool is_equal = (match == static_cast<int>(key.size()) && match == static_cast<int>(s_mid.size()));
                    if (is_equal && IsLowerBound) {
                        R = mid;
                        r = match;
                    } else {
                        L = mid;
                        l = match;
                    }
                }
            }
        }
        return R;
    }

    // Internal node upper_bound using the same lexicographic order as the API.
    // Returns first child index i such that pivot[i] > key.
    int upper_bound_pos(Node* n, const std::string& key) const {
        return lcp_binary_search<false, false>(n, key);
    }

    // Leaf lower_bound (first pos where keys[pos] >= key)
    int lower_bound_pos_leaf(Node* n, const std::string& key) const {
        return lcp_binary_search<true, true>(n, key);
    }

    // Leaf upper_bound (first pos where keys[pos] > key)
    int upper_bound_pos_leaf(Node* n, const std::string& key) const {
        return lcp_binary_search<true, false>(n, key);
    }

    // Rebuild weights_prefix for a leaf

    void rebuild_prefix(Node* n) {
        assert(n->is_leaf);
        n->weights_prefix[0] = 0;
        for (int i = 0; i < n->count; ++i) {
            n->weights_prefix[i+1] = n->weights_prefix[i] + n->weights[i];
        }
    }

    int leaf_total(Node* n) const {
        assert(n->is_leaf);
        return n->weights_prefix[n->count];
    }

    // Insert / update

    bool set_weight(const std::string& key, int w) {
        int old_w = find_weight(key);
        if (old_w == w) return false;

        if (old_w != 0) {
            if (old_w > 0) --inserted_count_;
            else if (old_w < 0) --deleted_count_;
            update_weight_inplace(root_, key, w);
        } else {
            ++size_;
            InsertResult res = insert_node(root_, key, w);
            if (res.split) {
                Node* new_root = new Node(false);
                new_root->count = 1;
                new_root->pivot_keys[0] = res.median_key;
                new_root->children[0]   = root_;
                new_root->children[1]   = res.new_node;
                new_root->subtree_sum[0] = compute_subtree_sum(root_);
                new_root->subtree_sum[1] = compute_subtree_sum(res.new_node);
                root_ = new_root;
            }
        }

        if (w > 0) ++inserted_count_;
        else if (w < 0) ++deleted_count_;
        return true;
    }

    void update_weight_inplace(Node* n, const std::string& key, int new_w) {
        if (n->is_leaf) {
            int pos = lower_bound_pos_leaf(n, key);
            if (pos < n->count && n->keys[pos] == key) {
                n->weights[pos] = new_w;
                rebuild_prefix(n);
            }
            return;
        }
        int pos = upper_bound_pos(n, key);
        update_weight_inplace(n->children[pos], key, new_w);
        n->subtree_sum[pos] = compute_subtree_sum(n->children[pos]);
    }

    int compute_subtree_sum(Node* n) const {
        if (n->is_leaf) return leaf_total(n);
        int total = 0;
        for (int i = 0; i <= n->count; ++i) total += n->subtree_sum[i];
        return total;
    }

    size_t memory_bytes_node(Node* n) const {
        if (!n) return 0;
        size_t total = sizeof(Node);
        if (n->is_leaf) {
            for (int i = 0; i < n->count; ++i) {
                total += n->keys[i].capacity();
            }
            return total;
        }
        for (int i = 0; i < n->count; ++i) {
            total += n->pivot_keys[i].capacity();
        }
        for (int i = 0; i <= n->count; ++i) {
            total += memory_bytes_node(n->children[i]);
        }
        return total;
    }

    bool validate_node_lcps(Node* n) const {
        if (!n) return true;
        if (n->is_leaf) {
            if (n->count > 0 && n->lcps[0] != 0) return false;
            for (int i = 1; i < n->count; ++i) {
                if (n->lcps[i] != compute_lcp(n->keys[i], n->keys[i-1])) {
                    return false;
                }
            }
            return true;
        } else {
            if (n->count > 0 && n->pivot_lcps[0] != 0) return false;
            for (int i = 1; i < n->count; ++i) {
                if (n->pivot_lcps[i] != compute_lcp(n->pivot_keys[i], n->pivot_keys[i-1])) {
                    return false;
                }
            }
            for (int i = 0; i <= n->count; ++i) {
                if (!validate_node_lcps(n->children[i])) return false;
            }
            return true;
        }
    }

    struct InsertResult {
        bool split = false;
        std::string median_key;
        Node* new_node = nullptr;
    };

    InsertResult insert_node(Node* n, const std::string& key, int w) {
        InsertResult res;
        if (n->is_leaf) {
            int pos = lower_bound_pos_leaf(n, key);
            for (int i = n->count; i > pos; --i) {
                n->keys[i]    = std::move(n->keys[i-1]);
                n->weights[i] = n->weights[i-1];
                n->lcps[i]    = n->lcps[i-1];
            }
            n->keys[pos]    = key;
            n->weights[pos] = w;
            ++n->count;
            n->lcps[pos] = (pos > 0) ? compute_lcp(n->keys[pos], n->keys[pos-1]) : 0;
            if (pos+1 < n->count) {
                n->lcps[pos+1] = compute_lcp(n->keys[pos+1], n->keys[pos]);
            }
            rebuild_prefix(n);

            if (n->count == 2*BTREE_B) {
                Node* right = new Node(true);
                int mid = BTREE_B;
                res.median_key = n->keys[mid];
                right->count = n->count - mid;
                for (int i = 0; i < right->count; ++i) {
                    right->keys[i]    = std::move(n->keys[mid+i]);
                    right->weights[i] = n->weights[mid+i];
                }
                n->count = mid;
                rebuild_prefix(n);
                rebuild_prefix(right);
                rebuild_lcps(n);
                rebuild_lcps(right);
                right->next_leaf = n->next_leaf;
                n->next_leaf = right;
                res.split    = true;
                res.new_node = right;
            }
            return res;
        }

        // Internal node
        int pos = upper_bound_pos(n, key);
        InsertResult child_res = insert_node(n->children[pos], key, w);
        n->subtree_sum[pos] = compute_subtree_sum(n->children[pos]);

        if (!child_res.split) return res;

        // Insert median pivot key and new child at pos+1
        for (int i = n->count; i > pos; --i) {
            n->pivot_keys[i]      = std::move(n->pivot_keys[i-1]);
            n->pivot_lcps[i]      = n->pivot_lcps[i-1];
            n->children[i+1]      = n->children[i];
            n->subtree_sum[i+1]   = n->subtree_sum[i];
        }
        n->pivot_keys[pos]      = child_res.median_key;
        n->children[pos+1]      = child_res.new_node;
        n->subtree_sum[pos+1]   = compute_subtree_sum(child_res.new_node);
        ++n->count;
        n->pivot_lcps[pos] = (pos > 0) ? compute_lcp(n->pivot_keys[pos], n->pivot_keys[pos-1]) : 0;
        if (pos+1 < n->count) {
            n->pivot_lcps[pos+1] = compute_lcp(n->pivot_keys[pos+1], n->pivot_keys[pos]);
        }

        if (n->count == 2*BTREE_B) {
            Node* right = new Node(false);
            int mid = BTREE_B;
            res.median_key = n->pivot_keys[mid];
            right->count = n->count - mid - 1;
            for (int i = 0; i < right->count; ++i) {
                right->pivot_keys[i]     = std::move(n->pivot_keys[mid+1+i]);
                right->children[i]       = n->children[mid+1+i];
                right->subtree_sum[i]    = n->subtree_sum[mid+1+i];
            }
            right->children[right->count]    = n->children[n->count];
            right->subtree_sum[right->count] = n->subtree_sum[n->count];
            n->count = mid;
            rebuild_lcps(n);
            rebuild_lcps(right);
            res.split    = true;
            res.new_node = right;
        }
        return res;
    }

    // Discard

    bool discard_weight(const std::string& key, int w) {
        int old_w = find_weight(key);
        if (old_w != w) return false;

        zero_weight(root_, key);
        --size_;
        if (w > 0) --inserted_count_;
        else if (w < 0) --deleted_count_;
        return true;
    }

    void zero_weight(Node* n, const std::string& key) {
        if (n->is_leaf) {
            int pos = lower_bound_pos_leaf(n, key);
            if (pos < n->count && n->keys[pos] == key) {
                for (int i = pos; i < n->count-1; ++i) {
                    n->keys[i]    = std::move(n->keys[i+1]);
                    n->weights[i] = n->weights[i+1];
                    n->lcps[i]    = n->lcps[i+1];
                }
                --n->count;
                n->lcps[pos] = (pos > 0 && pos < n->count) ? compute_lcp(n->keys[pos], n->keys[pos-1]) : 0;
                rebuild_prefix(n);
            }
            return;
        }
        int pos = upper_bound_pos(n, key);
        zero_weight(n->children[pos], key);
        n->subtree_sum[pos] = compute_subtree_sum(n->children[pos]);
    }

    // Range collection

    void collect_range(Node* n, const std::string& lo, const std::string& hi,
                       std::vector<std::pair<std::string,int>>& out) const {
        if (n->is_leaf) {
            int start = lower_bound_pos_leaf(n, lo);
            for (int i = start; i < n->count && n->keys[i] <= hi; ++i) {
                out.emplace_back(n->keys[i], n->weights[i]);
            }
            return;
        }
        int pos_lo = upper_bound_pos(n, lo);
        int pos_hi = upper_bound_pos(n, hi);
        for (int p = pos_lo; p <= pos_hi && p <= n->count; ++p) {
            collect_range(n->children[p], lo, hi, out);
        }
    }

    void collect_by_weight(Node* n, int w,
                           std::vector<std::string>& out) const {
        if (n->is_leaf) {
            for (int i = 0; i < n->count; ++i)
                if (n->weights[i] == w) out.push_back(n->keys[i]);
            return;
        }
        for (int i = 0; i <= n->count; ++i)
            collect_by_weight(n->children[i], w, out);
    }
};

}  // namespace hrtli
