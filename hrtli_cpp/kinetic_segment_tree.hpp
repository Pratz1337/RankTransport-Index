#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <shared_mutex>
#include <mutex>
#include <iostream>
#include <limits>
#include <map>
#include <cstdint>

namespace hrtli {

#ifndef HRTLI_KEY_TO_DOUBLE_DEFINED
#define HRTLI_KEY_TO_DOUBLE_DEFINED
// Helper: converts a key to double using the monotone scalarization
inline double hli_key_to_double(const std::string& path_str) {
    long double value = 0.0L;
    long double denom = 1024.0L;
    size_t limit = std::min<size_t>(path_str.size(), 24);
    for (size_t i = 0; i < limit; ++i) {
        unsigned int code = static_cast<unsigned char>(path_str[i]) + 1U;
        value += static_cast<long double>(code) / denom;
        denom *= 1024.0L;
    }
    return static_cast<double>(value);
}
#endif

#ifdef HRTLI_VERIFY_ORACLE
namespace v3 {
class RankTransportOracle {
private:
    std::vector<std::string> keys;

public:
    RankTransportOracle() {}

    void insert(const std::string& key) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        if (it == keys.end() || *it != key) {
            keys.insert(it, key);
        }
    }

    void remove(const std::string& key) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        if (it != keys.end() && *it == key) {
            keys.erase(it);
        }
    }

    bool contains(const std::string& key) const {
        return std::binary_search(keys.begin(), keys.end(), key);
    }

    uint32_t exact_rank(const std::string& key) const {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        return static_cast<uint32_t>(std::distance(keys.begin(), it));
    }
};
} // namespace v3
#endif

namespace v3 {

/**
 * @brief FeasibleEnvelope tracks constraints for the local linear models.
 * 
 * Algorithm: Incremental ShrinkingCone (O'Rourke 1981, adapted by PGM-index 2020).
 * - Each new point (x_j, y_j) adds 2 half-plane constraints.
 * - Maintains upper and lower bounds on the slope of the fitting line from the first point.
 * - Amortized O(1) per insertion.
 */
class FeasibleEnvelope {
public:
    FeasibleEnvelope() = default;

    void clear() {
        has_first_ = false;
        x0_ = 0.0;
        y0_ = 0.0;
        a_min_ = -std::numeric_limits<double>::infinity();
        a_max_ = std::numeric_limits<double>::infinity();
        n_points_ = 0;
    }

    /**
     * @brief Adds a point to the feasible region.
     * @return false if the feasible region becomes empty (meaning the leaf must split).
     */
    bool try_add_point(double x, double y, double eps) {
        if (!has_first_) {
            x0_ = x;
            y0_ = y;
            has_first_ = true;
            a_min_ = -std::numeric_limits<double>::infinity();
            a_max_ = std::numeric_limits<double>::infinity();
            n_points_ = 1;
            return true;
        }

        double dx = x - x0_;
        if (dx <= 1e-12) {
            // Point is duplicate or very close to start x coordinate
            return std::abs(y - y0_) <= eps;
        }

        double lower = (y - y0_ - eps) / dx;
        double upper = (y - y0_ + eps) / dx;

        double next_min = std::max(a_min_, lower);
        double next_max = std::min(a_max_, upper);

        if (next_min > next_max + 1e-12) {
            return false;
        }

        a_min_ = next_min;
        a_max_ = next_max;
        n_points_++;
        return true;
    }

    /**
     * @brief Computes a valid (a, b) model parameter pair from the feasible region bounds.
     */
    std::pair<double, double> get_model() const {
        double a = 0.0;
        if (a_min_ != -std::numeric_limits<double>::infinity() && 
            a_max_ != std::numeric_limits<double>::infinity()) {
            a = (a_min_ + a_max_) / 2.0;
        } else if (a_min_ != -std::numeric_limits<double>::infinity()) {
            a = a_min_;
        } else if (a_max_ != -std::numeric_limits<double>::infinity()) {
            a = a_max_;
        }
        double b = y0_ - a * x0_;
        return {a, b};
    }

    size_t size() const { return n_points_; }

    bool is_feasible() const {
        if (!has_first_) return true;
        return a_min_ <= a_max_ + 1e-12;
    }

private:
    bool has_first_ = false;
    double x0_ = 0.0;
    double y0_ = 0.0;
    double a_min_ = -std::numeric_limits<double>::infinity();
    double a_max_ = std::numeric_limits<double>::infinity();
    size_t n_points_ = 0;
};

/**
 * @brief Base Node structure for the Certified Kinetic Segment Tree.
 */
struct TreeNode {
    const bool is_leaf;
    uint64_t subtree_mass;

    TreeNode(bool leaf, uint64_t mass) : is_leaf(leaf), subtree_mass(mass) {}
    virtual ~TreeNode() = default;
};

using TreeNodePtr = std::shared_ptr<TreeNode>;

/**
 * @brief Buffered operation in the leaf write buffer.
 */
struct BufferedOp {
    std::string key;
    enum OpType { Insert, Erase } op;

    bool operator<(const BufferedOp& o) const { return key < o.key; }
};

// Compile-time SmallVector wrapper using std::vector
template <typename T, size_t N>
using SmallVector = std::vector<T>;

/**
 * @brief Leaf-local scalarization mapping a key remainder to [0, 1] relative to the leaf LCP.
 */
inline double local_scalarization(std::string_view key, std::string_view min_k, size_t lcp_len) {
    std::string_view remainder = key;
    if (key.size() >= lcp_len && key.compare(0, lcp_len, min_k, 0, lcp_len) == 0) {
        remainder = key.substr(lcp_len);
    }
    
    uint64_t val = 0;
    size_t limit = std::min<size_t>(remainder.size(), 8);
    for (size_t i = 0; i < limit; ++i) {
        val = (val << 8) | static_cast<unsigned char>(remainder[i]);
    }
    if (limit < 8) {
        val = val << (8 * (8 - limit));
    }
    
    return static_cast<double>(val) / 18446744073709551615.0;
}

/**
 * @brief LearnedLeaf stores local model parameters and local mutation/residual data.
 */
class alignas(64) LearnedLeaf : public TreeNode {
public:
    std::string min_key;
    std::string max_key;
    size_t lcp_len_ = 0;
    
    // Canonical sorted live keys in this leaf.
    std::vector<std::string> live_keys_;

    double a_ = 0.0;
    double b_ = 0.0;
    FeasibleEnvelope envelope_;
    bool is_fallback_ = false;

    // Small sorted write buffer (capacity ~32).
    SmallVector<BufferedOp, 32> write_buffer_;

    void update_lcp_len() {
        size_t l = 0;
        size_t max_l = std::min(min_key.size(), max_key.size());
        while (l < max_l && min_key[l] == max_key[l]) {
            l++;
        }
        lcp_len_ = l;
    }

    double local_scalarize(std::string_view key) const {
        std::string_view remainder = key;
        if (key.size() >= lcp_len_ && key.compare(0, lcp_len_, min_key, 0, lcp_len_) == 0) {
            remainder = key.substr(lcp_len_);
        }
        
        uint64_t val = 0;
        size_t limit = std::min<size_t>(remainder.size(), 8);
        for (size_t i = 0; i < limit; ++i) {
            val = (val << 8) | static_cast<unsigned char>(remainder[i]);
        }
        if (limit < 8) {
            val = val << (8 * (8 - limit));
        }
        
        return static_cast<double>(val) / 18446744073709551615.0;
    }

    size_t lcp_lower_bound(std::string_view target) const {
        size_t n = live_keys_.size();
        if (n == 0) return 0;
        
        // Left boundary check
        size_t l = 0;
        {
            size_t max_l = std::min(target.size(), live_keys_[0].size());
            while (l < max_l && target[l] == live_keys_[0][l]) {
                l++;
            }
        }
        if (l == target.size() || (l < live_keys_[0].size() && target[l] < live_keys_[0][l])) {
            return 0;
        }
        
        // Right boundary check
        size_t r = 0;
        {
            size_t max_r = std::min(target.size(), live_keys_[n - 1].size());
            while (r < max_r && target[r] == live_keys_[n - 1][r]) {
                r++;
            }
        }
        if (r == live_keys_[n - 1].size()) {
            if (r < target.size()) return n;
        } else if (r < target.size() && target[r] > live_keys_[n - 1][r]) {
            return n;
        }
        
        size_t L = 0;
        size_t R = n - 1;
        
        while (L + 1 < R) {
            size_t mid = L + (R - L) / 2;
            size_t match = std::min(l, r);
            
            size_t max_m = std::min(target.size(), live_keys_[mid].size());
            while (match < max_m && target[match] == live_keys_[mid][match]) {
                match++;
            }
            
            bool target_is_smaller = false;
            if (match == target.size()) {
                if (match == live_keys_[mid].size()) {
                    return mid;
                } else {
                    target_is_smaller = true;
                }
            } else if (match == live_keys_[mid].size()) {
                target_is_smaller = false;
            } else {
                target_is_smaller = (target[match] < live_keys_[mid][match]);
            }
            
            if (target_is_smaller) {
                R = mid;
                r = match;
            } else {
                L = mid;
                l = match;
            }
        }
        
        return R;
    }

    LearnedLeaf() : TreeNode(true, 0), is_fallback_(false) {
        update_lcp_len();
    }
    LearnedLeaf(const std::string& min_k, const std::string& max_k) 
        : TreeNode(true, 0), min_key(min_k), max_key(max_k), is_fallback_(false) {
        update_lcp_len();
    }

    /**
     * @brief Computes local rank within the leaf using local model + buffer.
     * Visibility precedence: write_buffer_ > live_keys_.
     */
    uint32_t local_rank(std::string_view k) const {
        size_t idx = lcp_lower_bound(k);
        int base_rank = static_cast<int>(idx);

        int delta = 0;
        auto end_it = std::lower_bound(write_buffer_.begin(), write_buffer_.end(), k,
            [](const BufferedOp& op, std::string_view val) { return op.key < val; });
        for (auto it = write_buffer_.begin(); it != end_it; ++it) {
            if (it->op == BufferedOp::Insert) {
                delta++;
            } else {
                delta--;
            }
        }
        return static_cast<uint32_t>(std::max(0, base_rank + delta));
    }

    /**
     * @brief Inserts a key into the write buffer. Returns false if split is needed.
     */
    bool insert(std::string_view k, size_t max_leaf_mass) {
        auto it = std::lower_bound(write_buffer_.begin(), write_buffer_.end(), k,
            [](const BufferedOp& op, std::string_view val) { return op.key < val; });
        if (it != write_buffer_.end() && it->key == k) {
            if (it->op == BufferedOp::Erase) {
                write_buffer_.erase(it);
                subtree_mass++;
            }
        } else {
            size_t idx = lcp_lower_bound(k);
            bool in_live = (idx < live_keys_.size() && live_keys_[idx] == k);
            if (!in_live) {
                write_buffer_.insert(it, {std::string(k), BufferedOp::Insert});
                subtree_mass++;
            }
        }
        return !must_split(max_leaf_mass);
    }

    /**
     * @brief Erases a key from the write buffer.
     */
    bool erase(std::string_view k) {
        auto it = std::lower_bound(write_buffer_.begin(), write_buffer_.end(), k,
            [](const BufferedOp& op, std::string_view val) { return op.key < val; });
        if (it != write_buffer_.end() && it->key == k) {
            if (it->op == BufferedOp::Insert) {
                write_buffer_.erase(it);
                if (subtree_mass > 0) subtree_mass--;
            }
        } else {
            size_t idx = lcp_lower_bound(k);
            bool in_live = (idx < live_keys_.size() && live_keys_[idx] == k);
            if (in_live) {
                write_buffer_.insert(it, {std::string(k), BufferedOp::Erase});
                if (subtree_mass > 0) subtree_mass--;
            }
        }
        return true;
    }

    bool must_split(size_t max_leaf_mass) const {
        return write_buffer_.size() > 32 || subtree_mass > max_leaf_mass;
    }

    bool may_merge_with(const LearnedLeaf& right) const {
        return (subtree_mass + right.subtree_mass) < 32;
    }

    /**
     * @brief Merges write buffer into live_keys_ and rebuilds local linear fit.
     */
    void materialize(double eps_g) {
        std::vector<std::string> merged;
        merged.reserve(live_keys_.size() + write_buffer_.size());
        
        auto live_it = live_keys_.begin();
        auto buf_it = write_buffer_.begin();
        
        while (live_it != live_keys_.end() || buf_it != write_buffer_.end()) {
            if (live_it != live_keys_.end() && buf_it != write_buffer_.end()) {
                if (*live_it < buf_it->key) {
                    merged.push_back(std::move(*live_it));
                    ++live_it;
                } else if (buf_it->key < *live_it) {
                    if (buf_it->op == BufferedOp::Insert) {
                        merged.push_back(std::move(buf_it->key));
                    }
                    ++buf_it;
                } else {
                    // keys are equal
                    if (buf_it->op == BufferedOp::Insert) {
                        merged.push_back(std::move(*live_it));
                    } else {
                        // Erase of an existing key - skip both
                    }
                    ++live_it;
                    ++buf_it;
                }
            } else if (live_it != live_keys_.end()) {
                merged.push_back(std::move(*live_it));
                ++live_it;
            } else {
                if (buf_it->op == BufferedOp::Insert) {
                    merged.push_back(std::move(buf_it->key));
                }
                ++buf_it;
            }
        }
        
        live_keys_ = std::move(merged);
        write_buffer_.clear();

        // Check for scalarization collapse
        bool collapsed = false;
        for (size_t i = 0; i + 1 < live_keys_.size(); ++i) {
            double x1 = local_scalarize(live_keys_[i]);
            double x2 = local_scalarize(live_keys_[i+1]);
            if (std::abs(x1 - x2) < 1e-12) {
                collapsed = true;
                break;
            }
        }

        // Rebuild envelope
        envelope_.clear();
        bool envelope_ok = true;
        if (!collapsed) {
            for (size_t i = 0; i < live_keys_.size(); ++i) {
                double x = local_scalarize(live_keys_[i]);
                if (!envelope_.try_add_point(x, static_cast<double>(i) + 0.5, eps_g - 0.5)) {
                    envelope_ok = false;
                    break;
                }
            }
        }

        if (collapsed || !envelope_ok || !envelope_.is_feasible()) {
            is_fallback_ = true;
            a_ = 0.0;
            b_ = 0.0;
        } else {
            is_fallback_ = false;
            auto model = envelope_.get_model();
            a_ = model.first;
            b_ = model.second;
        }
        subtree_mass = live_keys_.size();
    }
};

/**
 * @brief WarpNode represents internal nodes containing separators and child pointers.
 */
class alignas(64) WarpNode : public TreeNode {
public:
    std::vector<std::string> separators;
    std::vector<TreeNodePtr> children;
    std::vector<uint64_t> separator_fingerprints;
    std::vector<uint64_t> child_prefix_masses;
    size_t routing_lcp_len = 0;

    WarpNode() : TreeNode(false, 0) {}

    static size_t common_prefix_len(std::string_view lhs, std::string_view rhs) {
        size_t len = 0;
        size_t limit = std::min(lhs.size(), rhs.size());
        while (len < limit && lhs[len] == rhs[len]) {
            ++len;
        }
        return len;
    }

    static uint64_t fingerprint64(std::string_view key, size_t offset = 0) {
        uint64_t fp = 0;
        if (offset > key.size()) {
            offset = key.size();
        }
        std::string_view suffix = key.substr(offset);
        size_t limit = std::min<size_t>(suffix.size(), 8);
        for (size_t i = 0; i < limit; ++i) {
            fp = (fp << 8) | static_cast<unsigned char>(suffix[i]);
        }
        return fp << (8 * (8 - limit));
    }

    void rebuild_routing_metadata() {
        routing_lcp_len = separators.empty() ? 0 : separators.front().size();
        for (const auto& sep : separators) {
            routing_lcp_len = std::min(routing_lcp_len, common_prefix_len(separators.front(), sep));
        }

        separator_fingerprints.clear();
        separator_fingerprints.reserve(separators.size());
        for (const auto& sep : separators) {
            separator_fingerprints.push_back(fingerprint64(sep, routing_lcp_len));
        }

        child_prefix_masses.resize(children.size());
        uint64_t running = 0;
        for (size_t i = 0; i < children.size(); ++i) {
            child_prefix_masses[i] = running;
            running += children[i]->subtree_mass;
        }
        subtree_mass = running;
    }

    size_t route_index(std::string_view key) const {
        bool key_in_lcp = routing_lcp_len == 0 ||
                          (key.size() >= routing_lcp_len &&
                           key.compare(0, routing_lcp_len, separators.front(), 0, routing_lcp_len) == 0);
        if (!key_in_lcp || separator_fingerprints.size() != separators.size()) {
            size_t lo = 0;
            size_t hi = separators.size();
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (std::string_view(separators[mid]).compare(key) > 0) {
                    hi = mid;
                } else {
                    lo = mid + 1;
                }
            }
            return lo;
        }

        uint64_t key_fp = fingerprint64(key, routing_lcp_len);
        size_t lo = 0;
        size_t hi = separators.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint64_t sep_fp = separator_fingerprints[mid];
            bool sep_greater = sep_fp > key_fp ||
                               (sep_fp == key_fp && std::string_view(separators[mid]).compare(key) > 0);
            if (sep_greater) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        return lo;
    }

    size_t route_index_with_prefix(std::string_view key, uint64_t& prefix_mass) const {
        size_t idx = route_index(key);
        if (child_prefix_masses.size() == children.size()) {
            prefix_mass += child_prefix_masses[idx];
        } else {
            for (size_t i = 0; i < idx; ++i) {
                prefix_mass += children[i]->subtree_mass;
            }
        }
        return idx;
    }

    const TreeNode* route_ptr(std::string_view key, uint64_t& prefix_mass) const {
        size_t idx = route_index_with_prefix(key, prefix_mass);
        const TreeNode* child = children[idx].get();
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(child, 0, 1);
#endif
        return child;
    }

    TreeNodePtr route(std::string_view key, uint64_t& prefix_mass) const {
        size_t idx = route_index_with_prefix(key, prefix_mass);
        TreeNodePtr child = children[idx];
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(child.get(), 0, 1);
#endif
        return child;
    }
};

/**
 * @brief Certified Kinetic Segment Tree (v3 Architecture).
 */
class KineticSegmentTree {
public:
    KineticSegmentTree(double eps_g = 16.0, size_t max_leaf_mass = 64) 
        : epsilon_g_(eps_g), max_leaf_mass_(max_leaf_mass) {
        auto leaf = std::make_shared<LearnedLeaf>("", "");
        leaf->subtree_mass = 0;
        root_ = std::static_pointer_cast<TreeNode>(leaf);
    }

    ~KineticSegmentTree() = default;

    /**
     * @brief Lookup for the dynamic rank of a key.
     */
    uint32_t rank_lookup(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        const TreeNode* current = root_.get();
        uint64_t prefix_mass = 0;

        while (!current->is_leaf) {
            const auto* internal = static_cast<const WarpNode*>(current);
            current = internal->route_ptr(key, prefix_mass);
        }

        const auto* leaf = static_cast<const LearnedLeaf*>(current);
        uint32_t res = prefix_mass + leaf->local_rank(key);
#ifdef HRTLI_VERIFY_ORACLE
        assert(res == oracle.exact_rank(key));
#endif
        return res;
    }

    /**
     * @brief Lookup for the predicted rank of a key.
     */
    double predict(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        const TreeNode* current = root_.get();
        uint64_t prefix_mass = 0;

        while (!current->is_leaf) {
            const auto* internal = static_cast<const WarpNode*>(current);
            current = internal->route_ptr(key, prefix_mass);
        }

        const auto* leaf = static_cast<const LearnedLeaf*>(current);
        double pred_local = 0.0;
        if (leaf->is_fallback_ || leaf->live_keys_.empty()) {
            pred_local = leaf->local_rank(key);
        } else {
            if (key < leaf->live_keys_.front()) {
                pred_local = 0.0;
            } else if (key > leaf->live_keys_.back()) {
                pred_local = static_cast<double>(leaf->live_keys_.size());
            } else {
                double x = leaf->local_scalarize(key);
                pred_local = leaf->a_ * x + leaf->b_;
            }

            int delta = 0;
            auto end_it = std::lower_bound(leaf->write_buffer_.begin(), leaf->write_buffer_.end(), key,
                [](const BufferedOp& op, std::string_view val) { return op.key < val; });
            for (auto it = leaf->write_buffer_.begin(); it != end_it; ++it) {
                if (it->op == BufferedOp::Insert) {
                    delta++;
                } else {
                    delta--;
                }
            }
            pred_local += delta;
        }
        double global_pred = static_cast<double>(prefix_mass) + pred_local;
#ifdef HRTLI_VERIFY_ORACLE
        double exact = oracle.exact_rank(key);
        double err = std::abs(global_pred - exact);
        if (err > epsilon_g_ + 1e-9) {
            std::cerr << "Prediction error violation for key " << key 
                      << ": pred=" << global_pred << ", exact=" << exact 
                      << ", err=" << err << ", epsilon=" << epsilon_g_ << std::endl;
            std::cerr << "Leaf range: [" << leaf->min_key << ", " << leaf->max_key << "], lcp_len=" << leaf->lcp_len_ 
                      << ", a=" << leaf->a_ << ", b=" << leaf->b_ << ", x=" << leaf->local_scalarize(key) 
                      << ", prefix_mass=" << prefix_mass << ", leaf_mass=" << leaf->subtree_mass << std::endl;
            assert(false);
        }
#endif
        return global_pred;
    }

    /**
     * @brief In-place insert operation.
     */
    void insert(std::string_view key) {
        std::unique_lock<std::shared_mutex> lock(tree_mu_);
#ifdef HRTLI_VERIFY_ORACLE
        oracle.insert(std::string(key));
#endif
        TreeNodePtr split_node;
        std::string split_key;
        bool split = insert_recursive(root_, key, split_node, split_key);

        if (split) {
            auto new_root = std::make_shared<WarpNode>();
            new_root->separators.push_back(split_key);
            new_root->children.push_back(root_);
            new_root->children.push_back(split_node);
            new_root->rebuild_routing_metadata();
            root_ = new_root;
        }
    }

    /**
     * @brief In-place delete/remove operation.
     */
    void remove(std::string_view key) {
        std::unique_lock<std::shared_mutex> lock(tree_mu_);
#ifdef HRTLI_VERIFY_ORACLE
        oracle.remove(std::string(key));
#endif
        remove_recursive(root_, key);

        if (!root_->is_leaf) {
            auto* internal = static_cast<WarpNode*>(root_.get());
            if (internal->children.size() == 1) {
                root_ = internal->children[0];
            }
        }
    }

    bool validate_invariants() const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        if (!root_) return true;
        return validate_invariants_recursive(root_, "", "");
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        return root_ ? root_->subtree_mass : 0;
    }

    TreeNodePtr get_root() const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        return root_;
    }

    double get_epsilon_g() const {
        return epsilon_g_;
    }

    bool contains(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        const TreeNode* current = root_.get();
        while (current && !current->is_leaf) {
            const auto* internal = static_cast<const WarpNode*>(current);
            uint64_t prefix_mass = 0;
            current = internal->route_ptr(key, prefix_mass);
        }
        if (!current) return false;
        const auto* leaf = static_cast<const LearnedLeaf*>(current);
        auto it = std::lower_bound(leaf->write_buffer_.begin(), leaf->write_buffer_.end(), key,
            [](const BufferedOp& op, std::string_view val) { return op.key < val; });
        if (it != leaf->write_buffer_.end() && it->key == key) {
            return it->op == BufferedOp::Insert;
        }
        size_t idx = leaf->lcp_lower_bound(key);
        return idx < leaf->live_keys_.size() && leaf->live_keys_[idx] == key;
    }

    int lookup(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        const TreeNode* current = root_.get();
        uint64_t prefix_mass = 0;
        while (current && !current->is_leaf) {
            const auto* internal = static_cast<const WarpNode*>(current);
            current = internal->route_ptr(key, prefix_mass);
        }
        if (!current) return -1;

        const auto* leaf = static_cast<const LearnedLeaf*>(current);
        auto it = std::lower_bound(leaf->write_buffer_.begin(), leaf->write_buffer_.end(), key,
            [](const BufferedOp& op, std::string_view val) { return op.key < val; });
        if (it != leaf->write_buffer_.end() && it->key == key) {
            return it->op == BufferedOp::Insert
                       ? static_cast<int>(prefix_mass + leaf->local_rank(key))
                       : -1;
        }

        size_t idx = leaf->lcp_lower_bound(key);
        if (idx < leaf->live_keys_.size() && leaf->live_keys_[idx] == key) {
            return static_cast<int>(prefix_mass + leaf->local_rank(key));
        }
        return -1;
    }

    size_t mutation_count() const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        return count_buffered(root_);
    }

    void collect_keys(TreeNodePtr node, const std::string& lo, const std::string& hi, std::vector<std::string>& out) const {
        if (!node) return;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            std::vector<std::string> leaf_keys;
            for (const auto& k : leaf->live_keys_) {
                bool deleted = false;
                for (const auto& op : leaf->write_buffer_) {
                    if (op.key == k && op.op == BufferedOp::Erase) {
                        deleted = true;
                        break;
                    }
                }
                if (!deleted) leaf_keys.push_back(k);
            }
            for (const auto& op : leaf->write_buffer_) {
                if (op.op == BufferedOp::Insert) {
                    leaf_keys.push_back(op.key);
                }
            }
            std::sort(leaf_keys.begin(), leaf_keys.end());
            leaf_keys.erase(std::unique(leaf_keys.begin(), leaf_keys.end()), leaf_keys.end());
            for (const auto& k : leaf_keys) {
                if (k >= lo && k <= hi) {
                    out.push_back(k);
                }
            }
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            for (size_t i = 0; i < internal->children.size(); ++i) {
                bool overlap = true;
                if (i > 0 && internal->separators[i - 1] > hi) {
                    overlap = false;
                }
                if (i < internal->separators.size() && internal->separators[i] < lo) {
                    overlap = false;
                }
                if (overlap) {
                    collect_keys(internal->children[i], lo, hi, out);
                }
            }
        }
    }

    void compute_memory(TreeNodePtr node, size_t& base_keys_sz, size_t& model_sz, size_t& delta_sz) const {
        if (!node) return;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            base_keys_sz += leaf->live_keys_.size() * sizeof(std::string);
            for (const auto& k : leaf->live_keys_) {
                base_keys_sz += k.capacity();
            }
            
            model_sz += sizeof(LearnedLeaf);
            model_sz += leaf->min_key.capacity() + leaf->max_key.capacity();
            
            delta_sz += leaf->write_buffer_.capacity() * sizeof(BufferedOp);
            for (const auto& op : leaf->write_buffer_) {
                delta_sz += op.key.capacity();
            }
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            model_sz += sizeof(WarpNode);
            model_sz += internal->separators.size() * sizeof(std::string);
            for (const auto& s : internal->separators) {
                model_sz += s.capacity();
            }
            model_sz += internal->children.size() * sizeof(TreeNodePtr);
            for (const auto& child : internal->children) {
                compute_memory(child, base_keys_sz, model_sz, delta_sz);
            }
        }
    }

    size_t count_leaves(TreeNodePtr node) const {
        if (!node) return 0;
        if (node->is_leaf) {
            return 1;
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            size_t leaves = 0;
            for (const auto& child : internal->children) {
                leaves += count_leaves(child);
            }
            return leaves;
        }
    }

    void collect_all_keys(TreeNodePtr node, std::vector<std::string>& out) const {
        if (!node) return;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            std::vector<std::string> leaf_keys;
            for (const auto& k : leaf->live_keys_) {
                bool deleted = false;
                for (const auto& op : leaf->write_buffer_) {
                    if (op.key == k && op.op == BufferedOp::Erase) {
                        deleted = true;
                        break;
                    }
                }
                if (!deleted) leaf_keys.push_back(k);
            }
            for (const auto& op : leaf->write_buffer_) {
                if (op.op == BufferedOp::Insert) {
                    leaf_keys.push_back(op.key);
                }
            }
            std::sort(leaf_keys.begin(), leaf_keys.end());
            leaf_keys.erase(std::unique(leaf_keys.begin(), leaf_keys.end()), leaf_keys.end());
            out.insert(out.end(), leaf_keys.begin(), leaf_keys.end());
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            for (const auto& child : internal->children) {
                collect_all_keys(child, out);
            }
        }
    }

    double get_model_max_error_recursive(TreeNodePtr node) const {
        if (!node) return 0.0;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            if (leaf->is_fallback_ || leaf->live_keys_.empty()) {
                return 0.0;
            }
            double max_err = 0.0;
            for (size_t i = 0; i < leaf->live_keys_.size(); ++i) {
                double x = leaf->local_scalarize(leaf->live_keys_[i]);
                double pred = leaf->a_ * x + leaf->b_;
                double err = std::abs(pred - static_cast<double>(i));
                if (err > max_err) {
                    max_err = err;
                }
            }
            return max_err;
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            double max_err = 0.0;
            for (const auto& child : internal->children) {
                max_err = std::max(max_err, get_model_max_error_recursive(child));
            }
            return max_err;
        }
    }

    void consolidate_recursive(TreeNodePtr node) {
        if (!node) return;
        if (node->is_leaf) {
            auto* leaf = static_cast<LearnedLeaf*>(node.get());
            leaf->materialize(epsilon_g_);
        } else {
            auto* internal = static_cast<WarpNode*>(node.get());
            for (auto& child : internal->children) {
                consolidate_recursive(child);
            }
            internal->rebuild_routing_metadata();
        }
    }

    void consolidate() {
        std::unique_lock<std::shared_mutex> lock(tree_mu_);
        consolidate_recursive(root_);
    }

    size_t count_live_keys(TreeNodePtr node) const {
        if (!node) return 0;
        if (node->is_leaf) {
            return static_cast<const LearnedLeaf*>(node.get())->live_keys_.size();
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            size_t sum = 0;
            for (const auto& child : internal->children) {
                sum += count_live_keys(child);
            }
            return sum;
        }
    }

    int delta_before_recursive(TreeNodePtr node, const std::string& key) const {
        if (!node) return 0;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            int delta = 0;
            auto end_it = std::lower_bound(leaf->write_buffer_.begin(), leaf->write_buffer_.end(), key,
                [](const BufferedOp& op, std::string_view val) { return op.key < val; });
            for (auto it = leaf->write_buffer_.begin(); it != end_it; ++it) {
                if (it->op == BufferedOp::Insert) {
                    delta++;
                } else {
                    delta--;
                }
            }
            return delta;
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            int delta = 0;
            for (size_t i = 0; i < internal->children.size(); ++i) {
                bool overlap = true;
                if (i > 0 && internal->separators[i - 1] >= key) {
                    overlap = false;
                }
                if (overlap) {
                    delta += delta_before_recursive(internal->children[i], key);
                }
            }
            return delta;
        }
    }

    int delta_before(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(tree_mu_);
        return delta_before_recursive(root_, key);
    }

private:
    size_t count_buffered(TreeNodePtr node) const {
        if (!node) return 0;
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            return leaf->write_buffer_.size();
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            size_t sum = 0;
            for (const auto& child : internal->children) {
                sum += count_buffered(child);
            }
            return sum;
        }
    }

    mutable std::shared_mutex tree_mu_;
    TreeNodePtr root_;
    double epsilon_g_;
    size_t max_leaf_mass_;
#ifdef HRTLI_VERIFY_ORACLE
    mutable RankTransportOracle oracle;
#endif

    TreeNodePtr merge_leaves(const LearnedLeaf& left, const LearnedLeaf& right) {
        auto merged = std::make_shared<LearnedLeaf>(left.min_key, right.max_key);
        
        auto get_all_keys = [](const LearnedLeaf& leaf) {
            std::vector<std::string> leaf_keys;
            for (const auto& k : leaf.live_keys_) {
                bool deleted = false;
                for (const auto& op : leaf.write_buffer_) {
                    if (op.key == k && op.op == BufferedOp::Erase) {
                        deleted = true;
                        break;
                    }
                }
                if (!deleted) leaf_keys.push_back(k);
            }
            for (const auto& op : leaf.write_buffer_) {
                if (op.op == BufferedOp::Insert) {
                    leaf_keys.push_back(op.key);
                }
            }
            return leaf_keys;
        };

        auto left_keys = get_all_keys(left);
        auto right_keys = get_all_keys(right);

        std::vector<std::string> keys;
        keys.insert(keys.end(), left_keys.begin(), left_keys.end());
        keys.insert(keys.end(), right_keys.begin(), right_keys.end());

        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

        if (keys.size() > max_leaf_mass_) {
            return nullptr;
        }

        merged->live_keys_ = keys;
        merged->materialize(epsilon_g_);

        if (!merged->envelope_.is_feasible()) {
            return nullptr;
        }

        return std::static_pointer_cast<TreeNode>(merged);
    }

    TreeNodePtr merge_internal_nodes(const WarpNode& left, const WarpNode& right, const std::string& parent_sep) {
        if (left.children.size() + right.children.size() > 16) {
            return nullptr;
        }
        auto merged = std::make_shared<WarpNode>();
        merged->children = left.children;
        merged->children.insert(merged->children.end(), right.children.begin(), right.children.end());

        merged->separators = left.separators;
        merged->separators.push_back(parent_sep);
        merged->separators.insert(merged->separators.end(), right.separators.begin(), right.separators.end());

        merged->rebuild_routing_metadata();
        return std::static_pointer_cast<TreeNode>(merged);
    }

    bool insert_recursive(TreeNodePtr current, std::string_view key,
                          TreeNodePtr& out_split_node, std::string& out_split_key) {
        if (current->is_leaf) {
            auto* leaf = static_cast<LearnedLeaf*>(current.get());
            bool ok = leaf->insert(key, max_leaf_mass_);
            
            if (!ok) {
                leaf->materialize(epsilon_g_);

                // Check collapse split guard
                bool degenerate = false;
                if (leaf->live_keys_.size() >= 2) {
                    size_t mid = leaf->live_keys_.size() / 2;
                    double x_start = local_scalarization(leaf->live_keys_[0], leaf->min_key, leaf->lcp_len_);
                    double x_mid = local_scalarization(leaf->live_keys_[mid], leaf->min_key, leaf->lcp_len_);
                    double x_end = local_scalarization(leaf->live_keys_.back(), leaf->min_key, leaf->lcp_len_);
                    if (std::abs(x_start - x_mid) < 1e-12 || std::abs(x_mid - x_end) < 1e-12) {
                        degenerate = true;
                    }
                }

                if (leaf->subtree_mass > max_leaf_mass_ || (!leaf->envelope_.is_feasible() && !degenerate)) {
                    auto right = std::make_shared<LearnedLeaf>("", leaf->max_key);

                    auto live = leaf->live_keys_;
                    size_t mid = live.size() / 2;
                    right->min_key = live[mid];
                    leaf->max_key = live[mid];
                    leaf->update_lcp_len();
                    right->update_lcp_len();

                    leaf->live_keys_.clear();
                    for (const auto& k : live) {
                        if (k < right->min_key) {
                            leaf->live_keys_.push_back(k);
                        } else {
                            right->live_keys_.push_back(k);
                        }
                    }

                    leaf->materialize(epsilon_g_);
                    right->materialize(epsilon_g_);

                    out_split_node = right;
                    out_split_key = right->min_key;
                    return true;
                } else if (degenerate) {
                    leaf->is_fallback_ = true;
                    leaf->a_ = 0.0;
                    leaf->b_ = 0.0;
                }
            }
            return false;
        } else {
            auto* internal = static_cast<WarpNode*>(current.get());
            size_t route_idx = internal->route_index(key);

            TreeNodePtr child_split_node;
            std::string child_split_key;
            bool child_split = insert_recursive(internal->children[route_idx], key, child_split_node, child_split_key);

            if (child_split) {
                internal->children.insert(internal->children.begin() + route_idx + 1, child_split_node);
                internal->separators.insert(internal->separators.begin() + route_idx, child_split_key);

                if (internal->children.size() > 16) {
                    auto right = std::make_shared<WarpNode>();

                    size_t mid = internal->children.size() / 2;
                    out_split_key = internal->separators[mid - 1];

                    right->children.assign(internal->children.begin() + mid, internal->children.end());
                    right->separators.assign(internal->separators.begin() + mid, internal->separators.end());

                    internal->children.erase(internal->children.begin() + mid, internal->children.end());
                    internal->separators.erase(internal->separators.begin() + (mid - 1), internal->separators.end());

                    internal->rebuild_routing_metadata();
                    right->rebuild_routing_metadata();

                    out_split_node = right;
                    return true;
                }
            }

            internal->rebuild_routing_metadata();
            return false;
        }
    }

    bool remove_recursive(TreeNodePtr current, std::string_view key) {
        if (current->is_leaf) {
            auto* leaf = static_cast<LearnedLeaf*>(current.get());
            leaf->erase(key);
            
            if (leaf->write_buffer_.size() > 32) {
                leaf->materialize(epsilon_g_);
            }
            return false;
        } else {
            auto* internal = static_cast<WarpNode*>(current.get());
            size_t route_idx = internal->route_index(key);

            remove_recursive(internal->children[route_idx], key);

            // Merge or redistribute check if child becomes under-full
            TreeNodePtr child = internal->children[route_idx];
            bool underfull = false;
            if (child->is_leaf) {
                underfull = (child->subtree_mass < max_leaf_mass_ / 4);
            } else {
                const auto* child_int = static_cast<const WarpNode*>(child.get());
                underfull = (child_int->children.size() < 4);
            }

            if (underfull) {
                if (child->is_leaf) {
                    bool merged = false;
                    // Try right sibling
                    if (route_idx + 1 < internal->children.size()) {
                        const auto* right_sib = static_cast<const LearnedLeaf*>(internal->children[route_idx + 1].get());
                        auto merged_leaf = merge_leaves(*static_cast<const LearnedLeaf*>(child.get()), *right_sib);
                        if (merged_leaf) {
                            internal->children[route_idx] = merged_leaf;
                            internal->children.erase(internal->children.begin() + route_idx + 1);
                            internal->separators.erase(internal->separators.begin() + route_idx);
                            merged = true;
                        }
                    }
                    // Try left sibling
                    if (!merged && route_idx > 0) {
                        const auto* left_sib = static_cast<const LearnedLeaf*>(internal->children[route_idx - 1].get());
                        auto merged_leaf = merge_leaves(*left_sib, *static_cast<const LearnedLeaf*>(child.get()));
                        if (merged_leaf) {
                            internal->children[route_idx - 1] = merged_leaf;
                            internal->children.erase(internal->children.begin() + route_idx);
                            internal->separators.erase(internal->separators.begin() + route_idx - 1);
                        }
                    }
                } else {
                    auto* child_int = static_cast<WarpNode*>(child.get());
                    bool merged_or_redistributed = false;
                    
                    // Try right sibling internal node
                    if (route_idx + 1 < internal->children.size()) {
                        auto* right_sib = static_cast<WarpNode*>(internal->children[route_idx + 1].get());
                        if (child_int->children.size() + right_sib->children.size() <= 16) {
                            auto merged_node = merge_internal_nodes(*child_int, *right_sib, internal->separators[route_idx]);
                            if (merged_node) {
                                internal->children[route_idx] = merged_node;
                                internal->children.erase(internal->children.begin() + route_idx + 1);
                                internal->separators.erase(internal->separators.begin() + route_idx);
                                merged_or_redistributed = true;
                            }
                        } else {
                            // Redistribute from right sibling
                            size_t L = child_int->children.size();
                            size_t R = right_sib->children.size();
                            size_t target = (L + R) / 2;
                            size_t to_move = target - L;
                            
                            child_int->separators.push_back(internal->separators[route_idx]);
                            child_int->separators.insert(child_int->separators.end(), right_sib->separators.begin(), right_sib->separators.begin() + (to_move - 1));
                            child_int->children.insert(child_int->children.end(), right_sib->children.begin(), right_sib->children.begin() + to_move);
                            
                            internal->separators[route_idx] = right_sib->separators[to_move - 1];
                            
                            right_sib->separators.erase(right_sib->separators.begin(), right_sib->separators.begin() + to_move);
                            right_sib->children.erase(right_sib->children.begin(), right_sib->children.begin() + to_move);
                            
                            child_int->rebuild_routing_metadata();
                            right_sib->rebuild_routing_metadata();
                            
                            merged_or_redistributed = true;
                        }
                    }
                    
                    // Try left sibling internal node
                    if (!merged_or_redistributed && route_idx > 0) {
                        auto* left_sib = static_cast<WarpNode*>(internal->children[route_idx - 1].get());
                        if (left_sib->children.size() + child_int->children.size() <= 16) {
                            auto merged_node = merge_internal_nodes(*left_sib, *child_int, internal->separators[route_idx - 1]);
                            if (merged_node) {
                                internal->children[route_idx - 1] = merged_node;
                                internal->children.erase(internal->children.begin() + route_idx);
                                internal->separators.erase(internal->separators.begin() + route_idx - 1);
                                merged_or_redistributed = true;
                            }
                        } else {
                            // Redistribute from left sibling
                            size_t L = left_sib->children.size();
                            size_t R = child_int->children.size();
                            size_t target = (L + R) / 2;
                            size_t to_move = L - target;
                            
                            std::vector<std::string> new_seps;
                            new_seps.assign(left_sib->separators.end() - (to_move - 1), left_sib->separators.end());
                            new_seps.push_back(internal->separators[route_idx - 1]);
                            new_seps.insert(new_seps.end(), child_int->separators.begin(), child_int->separators.end());
                            child_int->separators = new_seps;
                            
                            std::vector<TreeNodePtr> new_kids;
                            new_kids.assign(left_sib->children.end() - to_move, left_sib->children.end());
                            new_kids.insert(new_kids.end(), child_int->children.begin(), child_int->children.end());
                            child_int->children = new_kids;
                            
                            internal->separators[route_idx - 1] = left_sib->separators[left_sib->separators.size() - to_move];
                            
                            left_sib->separators.erase(left_sib->separators.end() - to_move, left_sib->separators.end());
                            left_sib->children.erase(left_sib->children.end() - to_move, left_sib->children.end());
                            
                            left_sib->rebuild_routing_metadata();
                            child_int->rebuild_routing_metadata();
                            
                            merged_or_redistributed = true;
                        }
                    }
                }
            }

            internal->rebuild_routing_metadata();
            return false;
        }
    }

    bool validate_invariants_recursive(TreeNodePtr node, const std::string& min_k, const std::string& max_k) const {
        if (node->is_leaf) {
            const auto* leaf = static_cast<const LearnedLeaf*>(node.get());
            if (!min_k.empty() && leaf->min_key < min_k) return false;
            if (!max_k.empty() && leaf->max_key > max_k) return false;
            for (size_t i = 0; i + 1 < leaf->live_keys_.size(); ++i) {
                if (leaf->live_keys_[i] >= leaf->live_keys_[i+1]) return false;
            }
            if (!leaf->is_fallback_) {
                for (size_t i = 0; i + 1 < leaf->live_keys_.size(); ++i) {
                    double x1 = local_scalarization(leaf->live_keys_[i], leaf->min_key, leaf->lcp_len_);
                    double x2 = local_scalarization(leaf->live_keys_[i+1], leaf->min_key, leaf->lcp_len_);
                    if (x1 >= x2 - 1e-12) return false;
                }
                for (size_t i = 0; i < leaf->live_keys_.size(); ++i) {
                    double x = local_scalarization(leaf->live_keys_[i], leaf->min_key, leaf->lcp_len_);
                    double pred = leaf->a_ * x + leaf->b_;
                    if (std::abs(pred - static_cast<double>(i)) > epsilon_g_ + 1e-9) {
                        return false;
                    }
                }
            }
            return true;
        } else {
            const auto* internal = static_cast<const WarpNode*>(node.get());
            if (internal->children.empty()) return false;
            if (internal->separators.size() + 1 != internal->children.size()) return false;
            if (internal->separator_fingerprints.size() != internal->separators.size()) return false;
            if (internal->child_prefix_masses.size() != internal->children.size()) return false;
            size_t expected_lcp = internal->separators.empty() ? 0 : internal->separators.front().size();
            for (const auto& sep : internal->separators) {
                expected_lcp = std::min(expected_lcp, WarpNode::common_prefix_len(internal->separators.front(), sep));
            }
            if (internal->routing_lcp_len != expected_lcp) return false;
            for (size_t i = 0; i < internal->separators.size(); ++i) {
                if (internal->separator_fingerprints[i] !=
                    WarpNode::fingerprint64(internal->separators[i], internal->routing_lcp_len)) return false;
            }
            for (size_t i = 0; i + 1 < internal->separators.size(); ++i) {
                if (internal->separators[i] >= internal->separators[i+1]) return false;
            }
            uint64_t expected_mass = 0;
            for (size_t i = 0; i < internal->children.size(); ++i) {
                if (internal->child_prefix_masses[i] != expected_mass) return false;
                expected_mass += internal->children[i]->subtree_mass;
            }
            if (internal->subtree_mass != expected_mass) return false;
            for (size_t i = 0; i < internal->children.size(); ++i) {
                std::string child_min = (i == 0) ? min_k : internal->separators[i-1];
                std::string child_max = (i == internal->separators.size()) ? max_k : internal->separators[i];
                if (!validate_invariants_recursive(internal->children[i], child_min, child_max)) {
                    return false;
                }
            }
            return true;
        }
    }
};

} // namespace v3

// Pluggable comparison wrappers to support both HrtLiV2 and HrtLiV3 configurations
template <typename IndexImpl>
class HrtLi {
public:
    IndexImpl impl_;

    void insert(std::string_view k) {
        impl_.insert(k);
    }

    void erase(std::string_view k) {
        impl_.remove(k);
    }

    uint32_t rank(std::string_view k) const {
        return impl_.rank_lookup(std::string(k));
    }

    double predict(std::string_view k) const {
        return impl_.predict(std::string(k));
    }
};

class RankTransportIndexV3 {
private:
    v3::KineticSegmentTree tree_;
    int consolidation_count_ = 0;
    mutable std::shared_mutex rw_lock;

public:
    RankTransportIndexV3(const std::vector<std::string>& initial_keys, int eps)
        : tree_(eps, 64) {
        for (const auto& key : initial_keys) {
            tree_.insert(key);
        }
    }

    ~RankTransportIndexV3() = default;

    void wait_rebuild() {}

    size_t size() const {
        return tree_.size();
    }

    size_t base_size() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        return tree_.count_live_keys(tree_.get_root());
    }

    size_t mutation_count() const {
        return tree_.mutation_count();
    }

    double mutation_ratio() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        size_t base_sz = tree_.count_live_keys(tree_.get_root());
        size_t mut_cnt = tree_.mutation_count();
        if (base_sz == 0) {
            return mut_cnt == 0 ? 0.0 : std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(mut_cnt) / static_cast<double>(base_sz);
    }

    int consolidation_count() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        return consolidation_count_;
    }

    bool contains_base(const std::string& key) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        v3::TreeNodePtr current = tree_.get_root();
        while (current && !current->is_leaf) {
            const auto* internal = static_cast<const v3::WarpNode*>(current.get());
            uint64_t prefix_mass = 0;
            current = internal->route(key, prefix_mass);
        }
        if (!current) return false;
        const auto* leaf = static_cast<const v3::LearnedLeaf*>(current.get());
        size_t idx = leaf->lcp_lower_bound(key);
        return idx < leaf->live_keys_.size() && leaf->live_keys_[idx] == key;
    }

    bool contains(const std::string& key) const {
        return tree_.contains(key);
    }

    bool insert(const std::string& key) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        if (tree_.contains(key)) {
            return false;
        }
        tree_.insert(key);
        return true;
    }

    bool remove(const std::string& key) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        if (!tree_.contains(key)) {
            return false;
        }
        tree_.remove(key);
        return true;
    }

    int delta_before(const std::string& key) const {
        return tree_.delta_before(key);
    }

    int exact_rank(const std::string& key) const {
        int rank = tree_.lookup(key);
        if (rank < 0) {
            throw std::runtime_error("Key not found in RankTransportIndex: " + key);
        }
        return rank;
    }

    int lookup(const std::string& key) const {
        return tree_.lookup(key);
    }

    bool point_lookup(const std::string& key) const {
        return tree_.contains(key);
    }

    int count_range(const std::string& lo, const std::string& hi) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        if (lo > hi) return 0;
        int rk_lo = tree_.rank_lookup(lo);
        int rk_hi = tree_.rank_lookup(hi);
        if (tree_.contains(hi)) {
            rk_hi += 1;
        }
        return rk_hi - rk_lo;
    }

    std::vector<std::string> scan_range(const std::string& lo, const std::string& hi) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        if (lo > hi) return {};
        std::vector<std::string> out;
        tree_.collect_keys(tree_.get_root(), lo, hi, out);
        return out;
    }

    std::vector<std::string> snapshot_keys() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        std::vector<std::string> out;
        tree_.collect_all_keys(tree_.get_root(), out);
        return out;
    }

    size_t consolidate() {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        size_t mutations = tree_.mutation_count();
        tree_.consolidate();
        consolidation_count_++;
        return mutations;
    }

    bool should_consolidate(double threshold) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        if (threshold <= 0.0) {
            throw std::invalid_argument("consolidation threshold must be positive");
        }
        size_t mutations = tree_.mutation_count();
        size_t base_sz = tree_.count_live_keys(tree_.get_root());
        double ratio = base_sz == 0 ? (mutations == 0 ? 0.0 : std::numeric_limits<double>::infinity())
                                    : static_cast<double>(mutations) / static_cast<double>(base_sz);
        return mutations > 0 && ratio >= threshold;
    }

    bool maybe_consolidate(double threshold) {
        std::unique_lock<std::shared_mutex> lock(rw_lock);
        size_t mutations = tree_.mutation_count();
        size_t base_sz = tree_.count_live_keys(tree_.get_root());
        double ratio = base_sz == 0 ? (mutations == 0 ? 0.0 : std::numeric_limits<double>::infinity())
                                    : static_cast<double>(mutations) / static_cast<double>(base_sz);
        if (mutations > 0 && ratio >= threshold) {
            tree_.consolidate();
            consolidation_count_++;
            return true;
        }
        return false;
    }

    void get_memory_breakdown(size_t &base_keys_sz, size_t &model_sz, size_t &hpsfc_sz, size_t &delta_sz) const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        base_keys_sz = 0;
        model_sz = 0;
        hpsfc_sz = 0;
        delta_sz = 0;
        tree_.compute_memory(tree_.get_root(), base_keys_sz, model_sz, delta_sz);
    }

    size_t model_segments_count() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        return tree_.count_leaves(tree_.get_root());
    }

    int get_model_max_error() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        return static_cast<int>(std::round(tree_.get_model_max_error_recursive(tree_.get_root())));
    }

    size_t memory_bytes() const {
        std::shared_lock<std::shared_mutex> lock(rw_lock);
        size_t base_keys_sz = 0, model_sz = 0, hpsfc_sz = 0, delta_sz = 0;
        tree_.compute_memory(tree_.get_root(), base_keys_sz, model_sz, delta_sz);
        return base_keys_sz + model_sz + hpsfc_sz + delta_sz;
    }
};

class StaticBaseWithTransport {
public:
    void insert(std::string_view k) { (void)k; }
    void remove(std::string_view k) { (void)k; }
    uint32_t rank_lookup(const std::string& k) const { (void)k; return 0; }
    double predict(const std::string& k) const { (void)k; return 0.0; }
};

using HrtLiV2 = HrtLi<StaticBaseWithTransport>;
using HrtLiV3 = HrtLi<v3::KineticSegmentTree>;

} // namespace hrtli
