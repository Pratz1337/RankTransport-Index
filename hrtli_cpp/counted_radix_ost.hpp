/**
 * CountedRadixOST -- order-statistic radix tree for live string keys.
 *
 * Functionally equivalent exact-rank baseline: a path-compressed radix trie
 * stores the live set and answers rank / count-range from subtree masses.
 * It is not a learned index and does not preserve a frozen-model certificate.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace hrtli {

class CountedRadixOST {
public:
    CountedRadixOST() : root_(new Node("")) {}
    CountedRadixOST(const CountedRadixOST&) = delete;
    CountedRadixOST& operator=(const CountedRadixOST&) = delete;
    CountedRadixOST(CountedRadixOST&&) noexcept = default;
    CountedRadixOST& operator=(CountedRadixOST&&) noexcept = default;

    void reset() {
        root_.reset(new Node(""));
        size_ = 0;
    }

    int size() const { return size_; }
    bool empty() const { return size_ == 0; }
    bool contains(const std::string& key) const { return weight_of(key) == 1; }

    bool insert(const std::string& key) {
        if (weight_of(key) == 1) return false;
        assign(root_.get(), key, 0, 1);
        ++size_;
        return true;
    }

    bool remove(const std::string& key) {
        if (weight_of(key) != 1) return false;
        assign(root_.get(), key, 0, 0);
        --size_;
        return true;
    }

    void bulk_load(const std::vector<std::string>& keys) {
        reset();
        for (const auto& key : keys) insert(key);
    }

    int rank(const std::string& key) const {
        if (weight_of(key) != 1) {
            throw std::runtime_error("CountedRadixOST rank is defined only for live keys");
        }
        return prefix_sum_lt(root_.get(), key, 0);
    }

    int lookup(const std::string& key) const {
        if (weight_of(key) != 1) return -1;
        return prefix_sum_lt(root_.get(), key, 0);
    }

    bool point_lookup(const std::string& key) const { return contains(key); }

    int count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        return prefix_sum_le(hi) - prefix_sum_lt(root_.get(), lo, 0);
    }

    size_t memory_bytes() const { return memory_bytes_node(root_.get()); }

    bool validate_internal() const {
        auto checked = validate_node(root_.get());
        return checked.first && checked.second == size_;
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

    static size_t common_prefix(const std::string& edge, const std::string& key, size_t pos) {
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
        std::unique_ptr<Node> old_child = std::move(node->children[index]);
        std::unique_ptr<Node> split(new Node(old_child->edge.substr(0, matched)));
        old_child->edge.erase(0, matched);
        split->subtree_sum = old_child->subtree_sum;
        split->children.push_back(std::move(old_child));
        if (pos + matched == key.size()) {
            split->weight = new_weight;
            split->subtree_sum += new_weight;
        } else {
            std::unique_ptr<Node> fresh(new Node(key.substr(pos + matched)));
            fresh->weight = new_weight;
            fresh->subtree_sum = new_weight;
            unsigned char fresh_byte = first_byte(fresh.get());
            size_t fresh_index = child_lower_bound(split.get(), fresh_byte);
            split->children.insert(split->children.begin() + static_cast<std::ptrdiff_t>(fresh_index),
                                   std::move(fresh));
            split->subtree_sum += new_weight;
        }
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
            std::unique_ptr<Node> only = std::move(child->children.front());
            child->edge += only->edge;
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

    int prefix_sum_le(const std::string& key) const {
        return prefix_sum_lt(root_.get(), key, 0) + (weight_of(key) == 1 ? 1 : 0);
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
            auto result = validate_node(child.get());
            if (!result.first) return {false, 0};
            sum += result.second;
        }
        return {sum == node->subtree_sum, sum};
    }
};

}  // namespace hrtli
