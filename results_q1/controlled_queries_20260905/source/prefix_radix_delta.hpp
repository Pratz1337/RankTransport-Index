/**
 * PrefixRadixDelta -- path-compressed signed radix ledger.
 *
 * Each node stores the signed mass of its complete subtree.  A strict-prefix
 * rank correction is computed by walking the query bytes and summing terminal
 * weights and lexicographically earlier sibling subtrees.  Unlike a flat
 * ordered tree, shared string prefixes are represented once.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace hrtli {

class PrefixRadixDelta {
public:
    PrefixRadixDelta() : root_(new Node("")) {}
    PrefixRadixDelta(const PrefixRadixDelta&) = delete;
    PrefixRadixDelta& operator=(const PrefixRadixDelta&) = delete;
    PrefixRadixDelta(PrefixRadixDelta&&) noexcept = default;
    PrefixRadixDelta& operator=(PrefixRadixDelta&&) noexcept = default;

    void reset() {
        root_.reset(new Node(""));
        size_ = inserted_count_ = deleted_count_ = 0;
    }

    int size() const { return size_; }
    int mutation_count() const { return size_; }
    int inserted_size() const { return inserted_count_; }
    int deleted_size() const { return deleted_count_; }
    bool empty() const { return size_ == 0; }

    bool mark_inserted(const std::string& key) { return set_weight(key, +1); }
    bool mark_deleted(const std::string& key) { return set_weight(key, -1); }
    bool discard_inserted(const std::string& key) { return discard_weight(key, +1); }
    bool discard_deleted(const std::string& key) { return discard_weight(key, -1); }

    bool contains_inserted(const std::string& key) const { return weight_of(key) == +1; }
    bool contains_deleted(const std::string& key) const { return weight_of(key) == -1; }

    int prefix_delta(const std::string& key) const {
        return prefix_sum_lt(root_.get(), key, 0);
    }

    int prefix_delta_le(const std::string& key) const {
        return prefix_delta(key) + weight_of(key);
    }

    void iter_range(const std::string& lo, const std::string& hi,
                    std::vector<std::pair<std::string, int>>& out) const {
        std::string prefix;
        collect_range(root_.get(), prefix, lo, hi, out);
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> out;
        std::string prefix;
        collect_weight(root_.get(), prefix, +1, out);
        return out;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> out;
        std::string prefix;
        collect_weight(root_.get(), prefix, -1, out);
        return out;
    }

    size_t memory_bytes() const { return memory_bytes_node(root_.get()); }

    bool validate_internal_lcps() const {
        return validate_node(root_.get()).first;
    }

private:
    struct Node {
        explicit Node(std::string label) : edge(std::move(label)) {}
        std::string edge;
        int weight = 0;
        int subtree_sum = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    std::unique_ptr<Node> root_;
    int size_ = 0;
    int inserted_count_ = 0;
    int deleted_count_ = 0;

    static unsigned char first_byte(const Node* node) {
        return static_cast<unsigned char>(node->edge.front());
    }

    static size_t child_lower_bound(const Node* node, unsigned char byte) {
        return static_cast<size_t>(std::lower_bound(
            node->children.begin(), node->children.end(), byte,
            [](const std::unique_ptr<Node>& child, unsigned char value) {
                return first_byte(child.get()) < value;
            }) - node->children.begin());
    }

    static size_t common_prefix(const std::string& edge,
                                const std::string& key, size_t pos) {
        size_t n = std::min(edge.size(), key.size() - pos);
        size_t i = 0;
        while (i < n && static_cast<unsigned char>(edge[i]) ==
                        static_cast<unsigned char>(key[pos + i])) {
            ++i;
        }
        return i;
    }

    int weight_of(const std::string& key) const {
        const Node* node = root_.get();
        size_t pos = 0;
        while (pos < key.size()) {
            size_t index = child_lower_bound(node, static_cast<unsigned char>(key[pos]));
            if (index == node->children.size() ||
                first_byte(node->children[index].get()) != static_cast<unsigned char>(key[pos])) {
                return 0;
            }
            node = node->children[index].get();
            size_t matched = common_prefix(node->edge, key, pos);
            if (matched != node->edge.size()) return 0;
            pos += matched;
        }
        return node->weight;
    }

    bool set_weight(const std::string& key, int new_weight) {
        int old_weight = weight_of(key);
        if (old_weight == new_weight) return false;

        // Allocation failure must leave both the tree and its counters intact.
        assign(root_.get(), key, 0, new_weight);
        if (old_weight > 0) --inserted_count_;
        else if (old_weight < 0) --deleted_count_;
        if (new_weight > 0) ++inserted_count_;
        else if (new_weight < 0) ++deleted_count_;
        if (old_weight == 0 && new_weight != 0) ++size_;
        else if (old_weight != 0 && new_weight == 0) --size_;
        return true;
    }

    bool discard_weight(const std::string& key, int expected) {
        if (weight_of(key) != expected) return false;
        return set_weight(key, 0);
    }

    static int assign(Node* node, const std::string& key, size_t pos, int new_weight) {
        if (pos == key.size()) {
            int diff = new_weight - node->weight;
            node->weight = new_weight;
            node->subtree_sum += diff;
            return diff;
        }

        unsigned char byte = static_cast<unsigned char>(key[pos]);
        size_t index = child_lower_bound(node, byte);
        if (index == node->children.size() || first_byte(node->children[index].get()) != byte) {
            std::unique_ptr<Node> fresh(new Node(key.substr(pos)));
            fresh->weight = new_weight;
            fresh->subtree_sum = new_weight;
            node->children.insert(node->children.begin() + static_cast<std::ptrdiff_t>(index),
                                  std::move(fresh));
            node->subtree_sum += new_weight;
            return new_weight;
        }

        Node* child = node->children[index].get();
        size_t matched = common_prefix(child->edge, key, pos);
        if (matched == child->edge.size()) {
            int diff = assign(child, key, pos + matched, new_weight);
            node->subtree_sum += diff;
            normalize_child(node, index);
            return diff;
        }

        // Prepare every allocation before detaching or shortening the live edge.
        std::unique_ptr<Node> split(new Node(child->edge.substr(0, matched)));
        std::string suffix = child->edge.substr(matched);
        std::unique_ptr<Node> fresh;
        split->children.reserve(2);
        split->subtree_sum = child->subtree_sum + new_weight;
        if (pos + matched == key.size()) {
            split->weight = new_weight;
        } else {
            fresh.reset(new Node(key.substr(pos + matched)));
            fresh->weight = new_weight;
            fresh->subtree_sum = new_weight;
        }
        child->edge.swap(suffix);
        std::unique_ptr<Node> old_child = std::move(node->children[index]);
        if (fresh && first_byte(fresh.get()) < first_byte(old_child.get())) {
            split->children.push_back(std::move(fresh));
        }
        split->children.push_back(std::move(old_child));
        if (fresh) split->children.push_back(std::move(fresh));
        node->children[index] = std::move(split);
        node->subtree_sum += new_weight;
        return new_weight;
    }

    static void normalize_child(Node* parent, size_t index) {
        Node* child = parent->children[index].get();
        if (child->weight == 0 && child->children.empty()) {
            parent->children.erase(parent->children.begin() + static_cast<std::ptrdiff_t>(index));
            return;
        }
        if (child->weight == 0 && child->children.size() == 1) {
            // Compression is optional. Keep a valid unary node if allocation
            // fails after the logical mutation has already committed below it.
            std::string merged;
            try {
                merged = child->edge + child->children.front()->edge;
            } catch (const std::bad_alloc&) {
                return;
            }
            std::unique_ptr<Node> only = std::move(child->children.front());
            child->edge.swap(merged);
            child->weight = only->weight;
            child->subtree_sum = only->subtree_sum;
            child->children = std::move(only->children);
        }
    }

    static int prefix_sum_lt(const Node* node, const std::string& key, size_t pos) {
        if (pos == key.size()) return 0;

        int total = node->weight;
        unsigned char byte = static_cast<unsigned char>(key[pos]);
        size_t index = child_lower_bound(node, byte);
        for (size_t i = 0; i < index; ++i) total += node->children[i]->subtree_sum;
        if (index == node->children.size() || first_byte(node->children[index].get()) != byte) {
            return total;
        }

        const Node* child = node->children[index].get();
        size_t matched = common_prefix(child->edge, key, pos);
        if (matched == child->edge.size()) {
            return total + prefix_sum_lt(child, key, pos + matched);
        }
        if (pos + matched == key.size()) return total;
        if (static_cast<unsigned char>(child->edge[matched]) <
            static_cast<unsigned char>(key[pos + matched])) {
            total += child->subtree_sum;
        }
        return total;
    }

    static void collect_range(const Node* node, std::string& prefix,
                              const std::string& lo, const std::string& hi,
                              std::vector<std::pair<std::string, int>>& out) {
        size_t old_size = prefix.size();
        prefix += node->edge;
        if (node->weight != 0 && prefix >= lo && prefix <= hi) {
            out.emplace_back(prefix, node->weight);
        }
        for (const auto& child : node->children) {
            collect_range(child.get(), prefix, lo, hi, out);
        }
        prefix.resize(old_size);
    }

    static void collect_weight(const Node* node, std::string& prefix, int wanted,
                               std::vector<std::string>& out) {
        size_t old_size = prefix.size();
        prefix += node->edge;
        if (node->weight == wanted) out.push_back(prefix);
        for (const auto& child : node->children) {
            collect_weight(child.get(), prefix, wanted, out);
        }
        prefix.resize(old_size);
    }

    static size_t memory_bytes_node(const Node* node) {
        size_t total = sizeof(Node) + node->edge.capacity();
        total += node->children.capacity() * sizeof(std::unique_ptr<Node>);
        for (const auto& child : node->children) total += memory_bytes_node(child.get());
        return total;
    }

    static std::pair<bool, int> validate_node(const Node* node) {
        int sum = node->weight;
        int previous = -1;
        for (const auto& child : node->children) {
            if (child->edge.empty()) return {false, 0};
            int current = static_cast<int>(first_byte(child.get()));
            if (current <= previous) return {false, 0};
            previous = current;
            std::pair<bool, int> result = validate_node(child.get());
            if (!result.first) return {false, 0};
            sum += result.second;
        }
        return {sum == node->subtree_sum, sum};
    }
};

}  // namespace hrtli
