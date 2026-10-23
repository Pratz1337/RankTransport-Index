/**
 * HP-SFC: Hierarchical Prefix Sparse Fingerprint Code table.
 *
 * This C++ prototype mirrors the Python HP-SFC fast path. It uses a stable
 * 64-bit FNV-1a fingerprint over sparse-radix lexicode units, not the older
 * APNPE/Poincare theta angle. Expected O(1) lookup follows from ordinary
 * open-addressing assumptions at fixed load factor; exact dynamic ranks still
 * come from the rank-transport delta layer.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace hrtli {

inline uint64_t fnv1a_mix_byte(uint64_t hash, uint8_t byte) {
    hash ^= byte;
    hash *= 1099511628211ULL;
    return hash;
}

/**
 * Stable 64-bit fingerprint over raw key bytes.
 *
 * Previous double-byte (+1, hi/lo) encoding matched the sparse-radix lexicode
 * unit layout but doubled hash work without improving collision resistance for
 * open addressing. Raw FNV-1a on the string bytes is order-preserving for
 * fingerprint identity (same key => same hash) and ~2x cheaper on the hot path.
 * Terminator bytes keep empty-key / prefix collisions distinct from longer keys.
 */
inline uint64_t sparse_lexicode_hash(const std::string& key) {
    uint64_t hash = 14695981039346656037ULL;
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    const size_t n = key.size();
    size_t i = 0;
    // Process 8-byte chunks with a cheap mix for long hierarchical paths.
    for (; i + 8 <= n; i += 8) {
        uint64_t chunk = 0;
        std::memcpy(&chunk, data + i, 8);
        hash ^= chunk;
        hash *= 1099511628211ULL;
    }
    for (; i < n; ++i) {
        hash = fnv1a_mix_byte(hash, data[i]);
    }
    // Two zero terminator bytes preserve the previous domain separation.
    hash = fnv1a_mix_byte(hash, 0U);
    hash = fnv1a_mix_byte(hash, 0U);
    return hash;
}

class HPSFCTable {
public:
    struct Slot {
        uint64_t fingerprint = 0;
        int base_idx = -1;
        bool occupied = false;
    };

    HPSFCTable() = default;

    void build(const std::vector<std::string>& base_keys) {
        n_keys_ = base_keys.size();
        size_ = 1;
        while (size_ < static_cast<size_t>(n_keys_ / 0.45) + 2) {
            size_ <<= 1;
        }
        mask_ = size_ - 1;
        table_.assign(size_, Slot{});
        total_probes_ = 0;
        keys_inserted_ = 0;

        for (size_t i = 0; i < base_keys.size(); i++) {
            insert_slot(base_keys[i], static_cast<int>(i));
        }
    }

    int lookup(const std::string& key, const std::vector<std::string>& base_keys) const {
        uint64_t fingerprint = sparse_lexicode_hash(key);
        size_t slot = hash_key(fingerprint);
        size_t stride = hash_stride(fingerprint);
        size_t probes = 0;
        while (probes < size_) {
            const Slot& s = table_[slot];
            if (!s.occupied) break;
            if (s.fingerprint == fingerprint &&
                s.base_idx >= 0 &&
                static_cast<size_t>(s.base_idx) < base_keys.size() &&
                base_keys[static_cast<size_t>(s.base_idx)] == key) {
                return s.base_idx;
            }
            slot = (slot + stride) & mask_;
            ++probes;
        }
        return -1;
    }

    bool contains(const std::string& key, const std::vector<std::string>& base_keys) const {
        return lookup(key, base_keys) >= 0;
    }

    double avg_probes() const {
        return keys_inserted_ > 0 ? static_cast<double>(total_probes_) / keys_inserted_ : 0.0;
    }

    size_t table_size() const { return size_; }
    size_t key_count() const { return keys_inserted_; }

private:
    size_t size_ = 0;
    size_t mask_ = 0;
    size_t n_keys_ = 0;
    size_t total_probes_ = 0;
    size_t keys_inserted_ = 0;
    std::vector<Slot> table_;

    size_t hash_key(uint64_t fingerprint) const {
        return fingerprint & mask_;
    }

    size_t hash_stride(uint64_t fingerprint) const {
        uint64_t h = fingerprint ^ 0x9E3779B97F4A7C15ULL;
        return 1 + 2 * (h % ((size_ >> 1) > 0 ? (size_ >> 1) : 1));
    }

    void insert_slot(const std::string& key, int base_idx) {
        uint64_t fingerprint = sparse_lexicode_hash(key);
        size_t slot = hash_key(fingerprint);
        size_t stride = hash_stride(fingerprint);
        size_t probes = 0;
        while (table_[slot].occupied) {
            slot = (slot + stride) & mask_;
            ++probes;
            if (probes >= size_) {
                throw std::runtime_error("HP-SFC table is full");
            }
        }
        table_[slot] = Slot{fingerprint, base_idx, true};
        total_probes_ += probes;
        ++keys_inserted_;
    }
};

}  // namespace hrtli
