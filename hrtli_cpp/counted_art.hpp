/**
 * CountedArt -- adaptive radix tree with inline per-child subtree counts.
 *
 * Adapted from libart (Armon Dadgar, BSD-3-Clause; revision
 * 301046804af165269e37da6725f5a4aec9ecc881): the same Node4/16/48/256 layouts,
 * optimistic prefix compression with MAX_PREFIX_LEN partial bytes, lazy leaf
 * expansion and NUL-terminated keys.  The augmentation stores, next to every
 * child pointer, the number of keys in that child's subtree, so a strict rank
 * walk sums earlier sibling counts without dereferencing siblings.  A live-key
 * rank fuses the membership check into the same walk.
 *
 * Domain: keys must not contain a NUL byte (the terminator makes the key set
 * prefix-free, as in the measured libart adapter).  Counts are uint32_t, so the
 * structure is limited to fewer than 2^32 keys.
 */
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

namespace hrtli {

class CountedArt {
public:
    CountedArt() = default;
    CountedArt(const CountedArt&) = delete;
    CountedArt& operator=(const CountedArt&) = delete;
    ~CountedArt() { destroy(root_); }

    uint64_t size() const { return size_; }
    uint64_t inner_nodes() const { return inner_nodes_; }

    bool contains(const std::string& k) const {
        Key key(k);
        return search(key.bytes, key.len);
    }

    bool insert(std::string_view k) { return insert(std::string(k)); }

    bool insert(const std::string& k) {
        Key key(k);
        bool inserted = false;
        insert_rec(root_, &root_, key.bytes, key.len, 0, static_cast<uint32_t>(size_), inserted);
        if (inserted) ++size_;
        return inserted;
    }

    bool erase(const std::string& k) {
        Key key(k);
        Leaf* removed = erase_rec(root_, &root_, key.bytes, key.len, 0);
        if (!removed) return false;
        std::free(removed);
        --size_;
        return true;
    }

    // Strict rank of a live key; rejects a non-live key like the HRT-LI API.
    int64_t rank(const std::string& k) const {
        Key key(k);
        bool found = false;
        const int64_t r = rank_walk(key.bytes, key.len, found);
        if (!found) throw std::runtime_error("CountedArt rank is defined only for live keys");
        return r;
    }

    int64_t boundary_rank(const std::string& k) const {
        Key key(k);
        bool found = false;
        return rank_walk(key.bytes, key.len, found);
    }

    int64_t count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        Key a(lo), b(hi);
        bool found_lo = false, found_hi = false;
        const int64_t left = rank_walk(a.bytes, a.len, found_lo);
        const int64_t right = rank_walk(b.bytes, b.len, found_hi);
        return right - left + (found_hi ? 1 : 0);
    }

private:
    static constexpr uint32_t kMaxPrefix = 10;
    enum : uint8_t { N4 = 1, N16, N48, N256 };

    struct Node {
        uint32_t partial_len;
        uint8_t type;
        uint8_t num_children;
        unsigned char partial[kMaxPrefix];
    };
    struct Node4 { Node n; unsigned char keys[4]; Node* children[4]; uint32_t counts[4]; };
    struct Node16 { Node n; unsigned char keys[16]; Node* children[16]; uint32_t counts[16]; };
    struct Node48 { Node n; unsigned char keys[256]; Node* children[48]; uint32_t counts[48]; };
    struct Node256 { Node n; Node* children[256]; uint32_t counts[256]; };
    struct Leaf { uint32_t key_len; unsigned char key[1]; };

    // The std::string terminator supplies the NUL byte, like the measured libart adapter.
    struct Key {
        const unsigned char* bytes;
        int len;
        explicit Key(const std::string& k)
            : bytes(reinterpret_cast<const unsigned char*>(k.c_str())), len(static_cast<int>(k.size() + 1)) {}
    };

    Node* root_ = nullptr;
    uint64_t size_ = 0;
    uint64_t inner_nodes_ = 0;

    static bool is_leaf(const Node* x) { return reinterpret_cast<uintptr_t>(x) & 1; }
    static Leaf* leaf_raw(const Node* x) { return reinterpret_cast<Leaf*>(reinterpret_cast<uintptr_t>(x) & ~uintptr_t(1)); }
    static Node* tag_leaf(Leaf* l) { return reinterpret_cast<Node*>(reinterpret_cast<uintptr_t>(l) | 1); }

    Node* alloc(uint8_t type) {
        size_t bytes = type == N4 ? sizeof(Node4) : type == N16 ? sizeof(Node16) : type == N48 ? sizeof(Node48) : sizeof(Node256);
        Node* n = static_cast<Node*>(std::calloc(1, bytes));
        if (!n) throw std::bad_alloc();
        n->type = type;
        ++inner_nodes_;
        return n;
    }
    void release(Node* n) { std::free(n); --inner_nodes_; }

    static Leaf* make_leaf(const unsigned char* key, int len) {
        Leaf* l = static_cast<Leaf*>(std::malloc(sizeof(uint32_t) + static_cast<size_t>(len)));
        if (!l) throw std::bad_alloc();
        l->key_len = static_cast<uint32_t>(len);
        std::memcpy(l->key, key, static_cast<size_t>(len));
        return l;
    }

    void destroy(Node* n) {
        if (!n) return;
        if (is_leaf(n)) { std::free(leaf_raw(n)); return; }
        switch (n->type) {
            case N4: for (int i = 0; i < n->num_children; ++i) destroy(reinterpret_cast<Node4*>(n)->children[i]); break;
            case N16: for (int i = 0; i < n->num_children; ++i) destroy(reinterpret_cast<Node16*>(n)->children[i]); break;
            case N48: for (int i = 0; i < 48; ++i) destroy(reinterpret_cast<Node48*>(n)->children[i]); break;
            case N256: for (int i = 0; i < 256; ++i) destroy(reinterpret_cast<Node256*>(n)->children[i]); break;
        }
        release(n);
    }

    // Returns the slot holding the child for byte c and its count cell.
    static Node** find_child(Node* n, unsigned char c, uint32_t** count) {
        switch (n->type) {
            case N4: {
                auto* p = reinterpret_cast<Node4*>(n);
                for (int i = 0; i < n->num_children; ++i)
                    if (p->keys[i] == c) { *count = &p->counts[i]; return &p->children[i]; }
                return nullptr;
            }
            case N16: {
                auto* p = reinterpret_cast<Node16*>(n);
                for (int i = 0; i < n->num_children; ++i)
                    if (p->keys[i] == c) { *count = &p->counts[i]; return &p->children[i]; }
                return nullptr;
            }
            case N48: {
                auto* p = reinterpret_cast<Node48*>(n);
                int i = p->keys[c];
                if (!i) return nullptr;
                *count = &p->counts[i - 1];
                return &p->children[i - 1];
            }
            default: {
                auto* p = reinterpret_cast<Node256*>(n);
                if (!p->children[c]) return nullptr;
                *count = &p->counts[c];
                return &p->children[c];
            }
        }
    }

    static Leaf* minimum(const Node* n) {
        while (!is_leaf(n)) {
            switch (n->type) {
                case N4: n = reinterpret_cast<const Node4*>(n)->children[0]; break;
                case N16: n = reinterpret_cast<const Node16*>(n)->children[0]; break;
                case N48: {
                    auto* p = reinterpret_cast<const Node48*>(n);
                    int i = 0;
                    while (!p->keys[i]) ++i;
                    n = p->children[p->keys[i] - 1];
                    break;
                }
                default: {
                    auto* p = reinterpret_cast<const Node256*>(n);
                    int i = 0;
                    while (!p->children[i]) ++i;
                    n = p->children[i];
                }
            }
        }
        return leaf_raw(n);
    }

    static bool leaf_matches(const Leaf* l, const unsigned char* key, int len) {
        return l->key_len == static_cast<uint32_t>(len) && std::memcmp(l->key, key, static_cast<size_t>(len)) == 0;
    }

    static int check_prefix(const Node* n, const unsigned char* key, int len, int depth) {
        int max_cmp = std::min<int>(std::min<int>(n->partial_len, kMaxPrefix), len - depth);
        int i = 0;
        while (i < max_cmp && n->partial[i] == key[depth + i]) ++i;
        return i;
    }

    static int prefix_mismatch(const Node* n, const unsigned char* key, int len, int depth) {
        int max_cmp = std::min<int>(std::min<int>(kMaxPrefix, n->partial_len), len - depth);
        int i = 0;
        for (; i < max_cmp; ++i) if (n->partial[i] != key[depth + i]) return i;
        if (n->partial_len > kMaxPrefix) {
            const Leaf* l = minimum(n);
            max_cmp = std::min<int>(static_cast<int>(l->key_len), len) - depth;
            for (; i < max_cmp; ++i) if (l->key[i + depth] != key[depth + i]) return i;
        }
        return i;
    }

    bool search(const unsigned char* key, int len) const {
        Node* n = root_;
        int depth = 0;
        while (n) {
            if (is_leaf(n)) return leaf_matches(leaf_raw(n), key, len);
            if (n->partial_len) {
                if (check_prefix(n, key, len, depth) != static_cast<int>(std::min<uint32_t>(kMaxPrefix, n->partial_len)))
                    return false;
                depth += static_cast<int>(n->partial_len);
                if (depth >= len) return false;
            }
            uint32_t* unused;
            Node** child = find_child(n, key[depth], &unused);
            n = child ? *child : nullptr;
            ++depth;
        }
        return false;
    }

    // Strict rank of an arbitrary NUL-terminated key; found reports equality.
    int64_t rank_walk(const unsigned char* key, int len, bool& found) const {
        const Node* n = root_;
        int depth = 0;
        int64_t total = 0;
        uint64_t subtree = size_;
        found = false;
        while (n) {
            if (is_leaf(n)) {
                const Leaf* l = leaf_raw(n);
                const int c = std::memcmp(l->key, key, std::min<size_t>(l->key_len, static_cast<size_t>(len)));
                if (c == 0 && l->key_len == static_cast<uint32_t>(len)) { found = true; return total; }
                // NUL termination makes keys prefix-free, so c != 0 here.
                return total + (c < 0 ? 1 : 0);
            }
            if (n->partial_len) {
                const Leaf* min_leaf = nullptr;
                for (uint32_t i = 0; i < n->partial_len; ++i) {
                    unsigned char b;
                    if (i < kMaxPrefix) b = n->partial[i];
                    else {
                        if (!min_leaf) min_leaf = minimum(n);
                        b = min_leaf->key[depth + i];
                    }
                    if (depth + static_cast<int>(i) >= len) return total;  // unreachable for NUL-terminated keys
                    const unsigned char q = key[depth + i];
                    if (b != q) return total + (b < q ? static_cast<int64_t>(subtree) : 0);
                }
                depth += static_cast<int>(n->partial_len);
            }
            if (depth >= len) return total;
            const unsigned char c = key[depth];
            const Node* next = nullptr;
            switch (n->type) {
                case N4: {
                    auto* p = reinterpret_cast<const Node4*>(n);
                    for (int i = 0; i < n->num_children; ++i) {
                        if (p->keys[i] < c) total += p->counts[i];
                        else { if (p->keys[i] == c) { next = p->children[i]; subtree = p->counts[i]; } break; }
                    }
                    break;
                }
                case N16: {
                    auto* p = reinterpret_cast<const Node16*>(n);
                    for (int i = 0; i < n->num_children; ++i) {
                        if (p->keys[i] < c) total += p->counts[i];
                        else { if (p->keys[i] == c) { next = p->children[i]; subtree = p->counts[i]; } break; }
                    }
                    break;
                }
                case N48: {
                    auto* p = reinterpret_cast<const Node48*>(n);
                    for (int b = 0; b < c; ++b) if (p->keys[b]) total += p->counts[p->keys[b] - 1];
                    if (p->keys[c]) { next = p->children[p->keys[c] - 1]; subtree = p->counts[p->keys[c] - 1]; }
                    break;
                }
                default: {
                    auto* p = reinterpret_cast<const Node256*>(n);
                    for (int b = 0; b < c; ++b) total += p->counts[b];  // zero for empty slots
                    if (p->children[c]) { next = p->children[c]; subtree = p->counts[c]; }
                }
            }
            n = next;
            ++depth;
        }
        return total;
    }

    static void copy_header(Node* dest, const Node* src) {
        dest->num_children = src->num_children;
        dest->partial_len = src->partial_len;
        std::memcpy(dest->partial, src->partial, std::min<uint32_t>(kMaxPrefix, src->partial_len));
    }

    void add_child256(Node256* n, unsigned char c, Node* child, uint32_t count) {
        n->n.num_children++;
        n->children[c] = child;
        n->counts[c] = count;
    }
    void add_child48(Node48* n, Node** ref, unsigned char c, Node* child, uint32_t count) {
        if (n->n.num_children < 48) {
            int pos = 0;
            while (n->children[pos]) ++pos;
            n->children[pos] = child;
            n->counts[pos] = count;
            n->keys[c] = static_cast<unsigned char>(pos + 1);
            n->n.num_children++;
            return;
        }
        auto* grown = reinterpret_cast<Node256*>(alloc(N256));
        for (int i = 0; i < 256; ++i) {
            if (n->keys[i]) {
                grown->children[i] = n->children[n->keys[i] - 1];
                grown->counts[i] = n->counts[n->keys[i] - 1];
            }
        }
        copy_header(&grown->n, &n->n);
        *ref = &grown->n;
        release(&n->n);
        add_child256(grown, c, child, count);
    }
    void add_child16(Node16* n, Node** ref, unsigned char c, Node* child, uint32_t count) {
        if (n->n.num_children < 16) {
            int idx = 0;
            while (idx < n->n.num_children && n->keys[idx] < c) ++idx;
            const int tail = n->n.num_children - idx;
            std::memmove(n->keys + idx + 1, n->keys + idx, static_cast<size_t>(tail));
            std::memmove(n->children + idx + 1, n->children + idx, static_cast<size_t>(tail) * sizeof(Node*));
            std::memmove(n->counts + idx + 1, n->counts + idx, static_cast<size_t>(tail) * sizeof(uint32_t));
            n->keys[idx] = c;
            n->children[idx] = child;
            n->counts[idx] = count;
            n->n.num_children++;
            return;
        }
        auto* grown = reinterpret_cast<Node48*>(alloc(N48));
        std::memcpy(grown->children, n->children, sizeof(Node*) * n->n.num_children);
        std::memcpy(grown->counts, n->counts, sizeof(uint32_t) * n->n.num_children);
        for (int i = 0; i < n->n.num_children; ++i) grown->keys[n->keys[i]] = static_cast<unsigned char>(i + 1);
        copy_header(&grown->n, &n->n);
        *ref = &grown->n;
        release(&n->n);
        add_child48(grown, ref, c, child, count);
    }
    void add_child4(Node4* n, Node** ref, unsigned char c, Node* child, uint32_t count) {
        if (n->n.num_children < 4) {
            int idx = 0;
            while (idx < n->n.num_children && n->keys[idx] < c) ++idx;
            const int tail = n->n.num_children - idx;
            std::memmove(n->keys + idx + 1, n->keys + idx, static_cast<size_t>(tail));
            std::memmove(n->children + idx + 1, n->children + idx, static_cast<size_t>(tail) * sizeof(Node*));
            std::memmove(n->counts + idx + 1, n->counts + idx, static_cast<size_t>(tail) * sizeof(uint32_t));
            n->keys[idx] = c;
            n->children[idx] = child;
            n->counts[idx] = count;
            n->n.num_children++;
            return;
        }
        auto* grown = reinterpret_cast<Node16*>(alloc(N16));
        std::memcpy(grown->children, n->children, sizeof(Node*) * n->n.num_children);
        std::memcpy(grown->counts, n->counts, sizeof(uint32_t) * n->n.num_children);
        std::memcpy(grown->keys, n->keys, n->n.num_children);
        copy_header(&grown->n, &n->n);
        *ref = &grown->n;
        release(&n->n);
        add_child16(grown, ref, c, child, count);
    }
    void add_child(Node* n, Node** ref, unsigned char c, Node* child, uint32_t count) {
        switch (n->type) {
            case N4: return add_child4(reinterpret_cast<Node4*>(n), ref, c, child, count);
            case N16: return add_child16(reinterpret_cast<Node16*>(n), ref, c, child, count);
            case N48: return add_child48(reinterpret_cast<Node48*>(n), ref, c, child, count);
            default: return add_child256(reinterpret_cast<Node256*>(n), c, child, count);
        }
    }

    // `count` is the number of keys currently below n (the parent's count cell).
    void insert_rec(Node* n, Node** ref, const unsigned char* key, int len, int depth, uint32_t count, bool& inserted) {
        if (!n) {
            *ref = tag_leaf(make_leaf(key, len));
            inserted = true;
            return;
        }
        if (is_leaf(n)) {
            Leaf* l = leaf_raw(n);
            if (leaf_matches(l, key, len)) return;
            Leaf* l2 = make_leaf(key, len);
            auto* fresh = reinterpret_cast<Node4*>(alloc(N4));
            int lcp = 0;
            const int limit = std::min<int>(static_cast<int>(l->key_len), len) - depth;
            while (lcp < limit && l->key[depth + lcp] == l2->key[depth + lcp]) ++lcp;
            fresh->n.partial_len = static_cast<uint32_t>(lcp);
            std::memcpy(fresh->n.partial, key + depth, std::min<int>(kMaxPrefix, lcp));
            *ref = &fresh->n;
            add_child4(fresh, ref, l->key[depth + lcp], tag_leaf(l), 1);
            add_child4(fresh, ref, l2->key[depth + lcp], tag_leaf(l2), 1);
            inserted = true;
            return;
        }
        if (n->partial_len) {
            const int diff = prefix_mismatch(n, key, len, depth);
            if (static_cast<uint32_t>(diff) >= n->partial_len) {
                depth += static_cast<int>(n->partial_len);
            } else {
                auto* fresh = reinterpret_cast<Node4*>(alloc(N4));
                *ref = &fresh->n;
                fresh->n.partial_len = static_cast<uint32_t>(diff);
                std::memcpy(fresh->n.partial, n->partial, std::min<int>(kMaxPrefix, diff));
                if (n->partial_len <= kMaxPrefix) {
                    add_child4(fresh, ref, n->partial[diff], n, count);
                    n->partial_len -= static_cast<uint32_t>(diff + 1);
                    std::memmove(n->partial, n->partial + diff + 1, std::min<uint32_t>(kMaxPrefix, n->partial_len));
                } else {
                    n->partial_len -= static_cast<uint32_t>(diff + 1);
                    const Leaf* l = minimum(n);
                    add_child4(fresh, ref, l->key[depth + diff], n, count);
                    std::memcpy(n->partial, l->key + depth + diff + 1, std::min<uint32_t>(kMaxPrefix, n->partial_len));
                }
                add_child4(fresh, ref, key[depth + diff], tag_leaf(make_leaf(key, len)), 1);
                inserted = true;
                return;
            }
        }
        uint32_t* cell = nullptr;
        Node** child = find_child(n, key[depth], &cell);
        if (child) {
            insert_rec(*child, child, key, len, depth + 1, *cell, inserted);
            if (inserted) ++*cell;  // n itself is never replaced by a deeper change
            return;
        }
        add_child(n, ref, key[depth], tag_leaf(make_leaf(key, len)), 1);
        inserted = true;
    }

    void remove_child256(Node256* n, Node** ref, unsigned char c) {
        n->children[c] = nullptr;
        n->counts[c] = 0;
        n->n.num_children--;
        if (n->n.num_children == 37) {
            auto* shrunk = reinterpret_cast<Node48*>(alloc(N48));
            *ref = &shrunk->n;
            copy_header(&shrunk->n, &n->n);
            int pos = 0;
            for (int i = 0; i < 256; ++i) {
                if (n->children[i]) {
                    shrunk->children[pos] = n->children[i];
                    shrunk->counts[pos] = n->counts[i];
                    shrunk->keys[i] = static_cast<unsigned char>(pos + 1);
                    ++pos;
                }
            }
            release(&n->n);
        }
    }
    void remove_child48(Node48* n, Node** ref, unsigned char c) {
        int pos = n->keys[c];
        n->keys[c] = 0;
        n->children[pos - 1] = nullptr;
        n->counts[pos - 1] = 0;
        n->n.num_children--;
        if (n->n.num_children == 12) {
            auto* shrunk = reinterpret_cast<Node16*>(alloc(N16));
            *ref = &shrunk->n;
            copy_header(&shrunk->n, &n->n);
            int child = 0;
            for (int i = 0; i < 256; ++i) {
                pos = n->keys[i];
                if (pos) {
                    shrunk->keys[child] = static_cast<unsigned char>(i);
                    shrunk->children[child] = n->children[pos - 1];
                    shrunk->counts[child] = n->counts[pos - 1];
                    ++child;
                }
            }
            release(&n->n);
        }
    }
    void remove_child16(Node16* n, Node** ref, Node** slot) {
        const int pos = static_cast<int>(slot - n->children);
        const int tail = n->n.num_children - 1 - pos;
        std::memmove(n->keys + pos, n->keys + pos + 1, static_cast<size_t>(tail));
        std::memmove(n->children + pos, n->children + pos + 1, static_cast<size_t>(tail) * sizeof(Node*));
        std::memmove(n->counts + pos, n->counts + pos + 1, static_cast<size_t>(tail) * sizeof(uint32_t));
        n->n.num_children--;
        if (n->n.num_children == 3) {
            auto* shrunk = reinterpret_cast<Node4*>(alloc(N4));
            *ref = &shrunk->n;
            copy_header(&shrunk->n, &n->n);
            std::memcpy(shrunk->keys, n->keys, 4);
            std::memcpy(shrunk->children, n->children, 4 * sizeof(Node*));
            std::memcpy(shrunk->counts, n->counts, 4 * sizeof(uint32_t));
            release(&n->n);
        }
    }
    void remove_child4(Node4* n, Node** ref, Node** slot) {
        const int pos = static_cast<int>(slot - n->children);
        const int tail = n->n.num_children - 1 - pos;
        std::memmove(n->keys + pos, n->keys + pos + 1, static_cast<size_t>(tail));
        std::memmove(n->children + pos, n->children + pos + 1, static_cast<size_t>(tail) * sizeof(Node*));
        std::memmove(n->counts + pos, n->counts + pos + 1, static_cast<size_t>(tail) * sizeof(uint32_t));
        n->n.num_children--;
        if (n->n.num_children == 1) {
            Node* child = n->children[0];
            if (!is_leaf(child)) {
                uint32_t prefix = n->n.partial_len;
                if (prefix < kMaxPrefix) n->n.partial[prefix++] = n->keys[0];
                if (prefix < kMaxPrefix) {
                    const uint32_t sub = std::min<uint32_t>(child->partial_len, kMaxPrefix - prefix);
                    std::memcpy(n->n.partial + prefix, child->partial, sub);
                    prefix += sub;
                }
                std::memcpy(child->partial, n->n.partial, std::min<uint32_t>(prefix, kMaxPrefix));
                child->partial_len += n->n.partial_len + 1;
            }
            // The parent's count cell for this slot already equals the
            // remaining child's subtree size once the caller decrements it.
            *ref = child;
            release(&n->n);
        }
    }
    void remove_child(Node* n, Node** ref, unsigned char c, Node** slot) {
        switch (n->type) {
            case N4: return remove_child4(reinterpret_cast<Node4*>(n), ref, slot);
            case N16: return remove_child16(reinterpret_cast<Node16*>(n), ref, slot);
            case N48: return remove_child48(reinterpret_cast<Node48*>(n), ref, c);
            default: return remove_child256(reinterpret_cast<Node256*>(n), ref, c);
        }
    }

    Leaf* erase_rec(Node* n, Node** ref, const unsigned char* key, int len, int depth) {
        if (!n) return nullptr;
        if (is_leaf(n)) {
            Leaf* l = leaf_raw(n);
            if (!leaf_matches(l, key, len)) return nullptr;
            *ref = nullptr;
            return l;
        }
        if (n->partial_len) {
            if (check_prefix(n, key, len, depth) != static_cast<int>(std::min<uint32_t>(kMaxPrefix, n->partial_len)))
                return nullptr;
            depth += static_cast<int>(n->partial_len);
            if (depth >= len) return nullptr;
        }
        uint32_t* cell = nullptr;
        Node** child = find_child(n, key[depth], &cell);
        if (!child) return nullptr;
        if (is_leaf(*child)) {
            Leaf* l = leaf_raw(*child);
            if (!leaf_matches(l, key, len)) return nullptr;
            remove_child(n, ref, key[depth], child);
            return l;
        }
        Leaf* removed = erase_rec(*child, child, key, len, depth + 1);
        if (removed) --*cell;
        return removed;
    }
};

}  // namespace hrtli
