#pragma once

#include <string>
#include <random>
#include <memory>
#include <vector>

namespace hrtli {

class OrderStatisticTreap {
private:
    struct Node {
        std::string key;
        double priority;
        int size;
        Node* left;
        Node* right;

        Node(const std::string& k, double p)
            : key(k), priority(p), size(1), left(nullptr), right(nullptr) {}
    };

    Node* root;
    std::mt19937 rng;
    std::uniform_real_distribution<double> dist;

    int get_size(Node* n) const {
        return n ? n->size : 0;
    }

    void update_size(Node* n) {
        if (n) {
            n->size = 1 + get_size(n->left) + get_size(n->right);
        }
    }

    Node* rotate_right(Node* y) {
        Node* x = y->left;
        y->left = x->right;
        x->right = y;
        update_size(y);
        update_size(x);
        return x;
    }

    Node* rotate_left(Node* x) {
        Node* y = x->right;
        x->right = y->left;
        y->left = x;
        update_size(x);
        update_size(y);
        return y;
    }

    Node* insert(Node* n, const std::string& key, double priority, bool& inserted) {
        if (!n) {
            inserted = true;
            return new Node(key, priority);
        }
        if (key == n->key) {
            inserted = false;
            return n;
        }
        if (key < n->key) {
            n->left = insert(n->left, key, priority, inserted);
            if (n->left->priority < n->priority) {
                n = rotate_right(n);
            }
        } else {
            n->right = insert(n->right, key, priority, inserted);
            if (n->right->priority < n->priority) {
                n = rotate_left(n);
            }
        }
        update_size(n);
        return n;
    }

    Node* erase(Node* n, const std::string& key, bool& erased) {
        if (!n) {
            erased = false;
            return nullptr;
        }
        if (key < n->key) {
            n->left = erase(n->left, key, erased);
        } else if (key > n->key) {
            n->right = erase(n->right, key, erased);
        } else {
            erased = true;
            if (!n->left) {
                Node* temp = n->right;
                delete n;
                return temp;
            } else if (!n->right) {
                Node* temp = n->left;
                delete n;
                return temp;
            }
            if (n->left->priority < n->right->priority) {
                n = rotate_right(n);
                n->right = erase(n->right, key, erased);
            } else {
                n = rotate_left(n);
                n->left = erase(n->left, key, erased);
            }
        }
        update_size(n);
        return n;
    }

    void clear(Node* n) {
        if (n) {
            clear(n->left);
            clear(n->right);
            delete n;
        }
    }

    void to_list(Node* n, std::vector<std::string>& list) const {
        if (n) {
            to_list(n->left, list);
            list.push_back(n->key);
            to_list(n->right, list);
        }
    }

public:
    OrderStatisticTreap(unsigned int seed = 42)
        : root(nullptr), rng(seed), dist(0.0, 1.0) {}

    ~OrderStatisticTreap() {
        clear(root);
    }

    // Disable copy
    OrderStatisticTreap(const OrderStatisticTreap&) = delete;
    OrderStatisticTreap& operator=(const OrderStatisticTreap&) = delete;

    // Enable move
    OrderStatisticTreap(OrderStatisticTreap&& other) noexcept
        : root(other.root), rng(std::move(other.rng)), dist(std::move(other.dist)) {
        other.root = nullptr;
    }

    OrderStatisticTreap& operator=(OrderStatisticTreap&& other) noexcept {
        if (this != &other) {
            clear(root);
            root = other.root;
            rng = std::move(other.rng);
            dist = std::move(other.dist);
            other.root = nullptr;
        }
        return *this;
    }

    int size() const {
        return get_size(root);
    }

    bool empty() const {
        return root == nullptr;
    }

    void reset() {
        clear(root);
        root = nullptr;
    }

    bool contains(const std::string& key) const {
        Node* n = root;
        while (n) {
            if (key == n->key) return true;
            n = (key < n->key) ? n->left : n->right;
        }
        return false;
    }

    bool add(const std::string& key) {
        bool inserted = false;
        root = insert(root, key, dist(rng), inserted);
        return inserted;
    }

    bool discard(const std::string& key) {
        bool erased = false;
        root = erase(root, key, erased);
        return erased;
    }

    int rank(const std::string& key) const {
        Node* n = root;
        int r = 0;
        while (n) {
            if (key <= n->key) {
                n = n->left;
            } else {
                r += 1 + get_size(n->left);
                n = n->right;
            }
        }
        return r;
    }

    std::vector<std::string> to_list() const {
        std::vector<std::string> list;
        to_list(root, list);
        return list;
    }
};

class SignedDeltaTreap {
private:
    struct Node {
        std::string key;
        int weight;
        double priority;
        int size;
        int subtree_sum;
        int inserted_count;
        int deleted_count;
        Node* left;
        Node* right;

        Node(const std::string& k, int w, double p)
            : key(k), weight(w), priority(p), size(1), subtree_sum(w),
              inserted_count(w > 0 ? 1 : 0), deleted_count(w < 0 ? 1 : 0),
              left(nullptr), right(nullptr) {}
    };

    Node* root;
    std::mt19937 rng;
    std::uniform_real_distribution<double> dist;

    int get_size(Node* n) const { return n ? n->size : 0; }
    int get_sum(Node* n) const { return n ? n->subtree_sum : 0; }
    int get_inserted_count(Node* n) const { return n ? n->inserted_count : 0; }
    int get_deleted_count(Node* n) const { return n ? n->deleted_count : 0; }

    void update(Node* n) {
        if (n) {
            n->size = 1 + get_size(n->left) + get_size(n->right);
            n->subtree_sum = n->weight + get_sum(n->left) + get_sum(n->right);
            n->inserted_count = (n->weight > 0 ? 1 : 0) + get_inserted_count(n->left) + get_inserted_count(n->right);
            n->deleted_count = (n->weight < 0 ? 1 : 0) + get_deleted_count(n->left) + get_deleted_count(n->right);
        }
    }

    Node* rotate_right(Node* y) {
        Node* x = y->left;
        y->left = x->right;
        x->right = y;
        update(y);
        update(x);
        return x;
    }

    Node* rotate_left(Node* x) {
        Node* y = x->right;
        x->right = y->left;
        y->left = x;
        update(x);
        update(y);
        return y;
    }

    Node* find_node(const std::string& key) const {
        Node* n = root;
        while (n) {
            if (key == n->key) return n;
            n = key < n->key ? n->left : n->right;
        }
        return nullptr;
    }

    Node* insert_or_update(Node* n, const std::string& key, int weight, double priority, bool& changed) {
        if (!n) {
            changed = true;
            return new Node(key, weight, priority);
        }
        if (key == n->key) {
            if (n->weight == weight) {
                changed = false;
                return n;
            }
            n->weight = weight;
            changed = true;
            update(n);
            return n;
        }
        if (key < n->key) {
            n->left = insert_or_update(n->left, key, weight, priority, changed);
            if (n->left->priority < n->priority) {
                n = rotate_right(n);
            }
        } else {
            n->right = insert_or_update(n->right, key, weight, priority, changed);
            if (n->right->priority < n->priority) {
                n = rotate_left(n);
            }
        }
        update(n);
        return n;
    }

    Node* erase_weight(Node* n, const std::string& key, int weight, bool& erased) {
        if (!n) {
            erased = false;
            return nullptr;
        }
        if (key < n->key) {
            n->left = erase_weight(n->left, key, weight, erased);
        } else if (key > n->key) {
            n->right = erase_weight(n->right, key, weight, erased);
        } else {
            if (n->weight != weight) {
                erased = false;
                return n;
            }
            erased = true;
            if (!n->left) {
                Node* temp = n->right;
                delete n;
                return temp;
            }
            if (!n->right) {
                Node* temp = n->left;
                delete n;
                return temp;
            }
            if (n->left->priority < n->right->priority) {
                n = rotate_right(n);
                n->right = erase_weight(n->right, key, weight, erased);
            } else {
                n = rotate_left(n);
                n->left = erase_weight(n->left, key, weight, erased);
            }
        }
        update(n);
        return n;
    }

    void clear(Node* n) {
        if (n) {
            clear(n->left);
            clear(n->right);
            delete n;
        }
    }

    void collect(Node* n, int weight, std::vector<std::string>& keys) const {
        if (n) {
            collect(n->left, weight, keys);
            if (n->weight == weight) {
                keys.push_back(n->key);
            }
            collect(n->right, weight, keys);
        }
    }

    bool set_weight(const std::string& key, int weight) {
        bool changed = false;
        root = insert_or_update(root, key, weight, dist(rng), changed);
        return changed;
    }

    bool discard_weight(const std::string& key, int weight) {
        bool erased = false;
        root = erase_weight(root, key, weight, erased);
        return erased;
    }

public:
    SignedDeltaTreap(unsigned int seed = 42)
        : root(nullptr), rng(seed), dist(0.0, 1.0) {}

    ~SignedDeltaTreap() {
        clear(root);
    }

    SignedDeltaTreap(const SignedDeltaTreap&) = delete;
    SignedDeltaTreap& operator=(const SignedDeltaTreap&) = delete;

    SignedDeltaTreap(SignedDeltaTreap&& other) noexcept
        : root(other.root), rng(std::move(other.rng)), dist(std::move(other.dist)) {
        other.root = nullptr;
    }

    SignedDeltaTreap& operator=(SignedDeltaTreap&& other) noexcept {
        if (this != &other) {
            clear(root);
            root = other.root;
            rng = std::move(other.rng);
            dist = std::move(other.dist);
            other.root = nullptr;
        }
        return *this;
    }

    int size() const { return get_size(root); }
    int inserted_size() const { return get_inserted_count(root); }
    int deleted_size() const { return get_deleted_count(root); }
    int mutation_count() const { return size(); }

    void reset() {
        clear(root);
        root = nullptr;
    }

    bool contains_inserted(const std::string& key) const {
        Node* n = find_node(key);
        return n && n->weight > 0;
    }

    bool contains_deleted(const std::string& key) const {
        Node* n = find_node(key);
        return n && n->weight < 0;
    }

    bool mark_inserted(const std::string& key) { return set_weight(key, 1); }
    bool mark_deleted(const std::string& key) { return set_weight(key, -1); }
    bool discard_inserted(const std::string& key) { return discard_weight(key, 1); }
    bool discard_deleted(const std::string& key) { return discard_weight(key, -1); }

    int prefix_delta(const std::string& key) const {
        Node* n = root;
        int total = 0;
        while (n) {
            if (key <= n->key) {
                n = n->left;
            } else {
                total += get_sum(n->left) + n->weight;
                n = n->right;
            }
        }
        return total;
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> keys;
        collect(root, 1, keys);
        return keys;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> keys;
        collect(root, -1, keys);
        return keys;
    }

    int prefix_delta_le(const std::string& key) const {
        Node* n = root;
        int total = 0;
        while (n) {
            if (key < n->key) {
                n = n->left;
            } else {
                total += get_sum(n->left) + n->weight;
                n = n->right;
            }
        }
        return total;
    }

    // Bounded in-order traversal: collect entries with lo <= key <= hi
    void iter_range(const std::string& lo, const std::string& hi,
                    std::vector<std::pair<std::string, int>>& out) const {
        iter_range_impl(root, lo, hi, out);
    }

private:
    void iter_range_impl(Node* n, const std::string& lo, const std::string& hi,
                         std::vector<std::pair<std::string, int>>& out) const {
        if (!n) return;
        if (n->key > lo) {
            iter_range_impl(n->left, lo, hi, out);
        }
        if (n->key >= lo && n->key <= hi) {
            out.emplace_back(n->key, n->weight);
        }
        if (n->key < hi) {
            iter_range_impl(n->right, lo, hi, out);
        }
    }
};

} // namespace hrtli

