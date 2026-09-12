#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "approx_local_delta.hpp"
#include "prefix_radix_delta.hpp"

namespace hrtli {

class MappedKeyStore {
public:
    MappedKeyStore(const std::string& path, size_t expected_count)
        : fd_(::open(path.c_str(), O_RDONLY)) {
        if (fd_ < 0) throw std::runtime_error("cannot open " + path);
        struct stat info {};
        if (::fstat(fd_, &info) != 0) {
            ::close(fd_);
            throw std::runtime_error("cannot stat " + path);
        }
        bytes_ = static_cast<size_t>(info.st_size);
        if (bytes_ == 0 && expected_count == 0) {
            offsets_.push_back(0);
            return;
        }
        data_ = static_cast<const char*>(
            ::mmap(nullptr, bytes_, PROT_READ, MAP_PRIVATE, fd_, 0));
        if (data_ == reinterpret_cast<const char*>(MAP_FAILED)) {
            data_ = nullptr;
            ::close(fd_);
            throw std::runtime_error("cannot mmap " + path);
        }
        ::madvise(const_cast<char*>(data_), bytes_, MADV_SEQUENTIAL);
        offsets_.reserve(expected_count + 1);
        offsets_.push_back(0);
        for (size_t i = 0; i < bytes_; ++i) {
            if (data_[i] == '\n') offsets_.push_back(i + 1);
        }
        if (offsets_.size() != expected_count + 1 || offsets_.back() != bytes_) {
            ::munmap(const_cast<char*>(data_), bytes_);
            data_ = nullptr;
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error(
                path + " does not contain the expected newline-terminated key count");
        }
    }

    ~MappedKeyStore() {
        if (data_) ::munmap(const_cast<char*>(data_), bytes_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedKeyStore(const MappedKeyStore&) = delete;
    MappedKeyStore& operator=(const MappedKeyStore&) = delete;

    size_t size() const { return offsets_.size() - 1; }
    size_t mapped_bytes() const { return bytes_; }
    size_t offsets_bytes() const { return offsets_.capacity() * sizeof(uint64_t); }

    std::string_view view(size_t index) const {
        const size_t start = offsets_[index];
        return std::string_view(data_ + start, offsets_[index + 1] - start - 1);
    }

    void advise_random() const {
        if (data_) ::madvise(const_cast<char*>(data_), bytes_, MADV_RANDOM);
    }

    bool strictly_sorted_unique() const {
        for (size_t i = 1; i < size(); ++i) {
            if (view(i - 1).compare(view(i)) >= 0) return false;
        }
        return true;
    }

    size_t lower_bound(std::string_view key) const {
        size_t lo = 0;
        size_t hi = size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (view(mid).compare(key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    size_t upper_bound(std::string_view key) const {
        size_t lo = 0;
        size_t hi = size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (view(mid).compare(key) <= 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    bool contains(std::string_view key) const {
        const size_t index = lower_bound(key);
        return index < size() && view(index) == key;
    }

private:
    int fd_ = -1;
    const char* data_ = nullptr;
    size_t bytes_ = 0;
    std::vector<uint64_t> offsets_;
};

inline uint64_t packed_sparse_hash(std::string_view key) {
    uint64_t hash = 14695981039346656037ULL;
    const auto* data = reinterpret_cast<const uint8_t*>(key.data());
    size_t i = 0;
    for (; i + 8 <= key.size(); i += 8) {
        uint64_t chunk = 0;
        std::memcpy(&chunk, data + i, 8);
        hash ^= chunk;
        hash *= 1099511628211ULL;
    }
    for (; i < key.size(); ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    hash ^= 0U;
    hash *= 1099511628211ULL;
    hash ^= 0U;
    hash *= 1099511628211ULL;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

inline double packed_key_to_double(std::string_view key) {
    double value = 0.0;
    double denominator = 1024.0;
    const size_t limit = std::min<size_t>(key.size(), 24);
    for (size_t i = 0; i < limit; ++i) {
        const unsigned int code = static_cast<unsigned char>(key[i]) + 1U;
        value += static_cast<double>(code) / denominator;
        denominator *= 1024.0;
    }
    return value;
}

class PackedFingerprintTable {
public:
#pragma pack(push, 1)
    struct Slot {
        uint64_t fingerprint;
        int32_t base_index;
    };
#pragma pack(pop)
    static_assert(sizeof(Slot) == 12, "packed fingerprint slot must remain 12 bytes");

    void build(const MappedKeyStore& keys) {
        if (keys.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            throw std::runtime_error("packed fingerprint index exceeds int32 capacity");
        }
        size_ = 1;
        while (size_ < static_cast<size_t>(keys.size() / 0.75) + 2) size_ <<= 1;
        mask_ = size_ - 1;
        table_.assign(size_, Slot{0, -1});
        total_probes_ = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            insert(keys.view(i), static_cast<int32_t>(i));
        }
    }

    int32_t lookup(std::string_view key, const MappedKeyStore& keys) const {
        const uint64_t fingerprint = packed_sparse_hash(key);
        size_t slot = fingerprint & mask_;
        const size_t stride = hash_stride(fingerprint);
        for (size_t probes = 0; probes < size_; ++probes) {
            const Slot& candidate = table_[slot];
            if (candidate.base_index < 0) return -1;
            if (candidate.fingerprint == fingerprint &&
                keys.view(static_cast<size_t>(candidate.base_index)) == key) {
                return candidate.base_index;
            }
            slot = (slot + stride) & mask_;
        }
        return -1;
    }

    size_t capacity() const { return size_; }
    size_t memory_bytes() const { return table_.capacity() * sizeof(Slot); }
    double average_build_probes(size_t key_count) const {
        return key_count == 0 ? 0.0 : static_cast<double>(total_probes_) / key_count;
    }

private:
    size_t size_ = 0;
    size_t mask_ = 0;
    uint64_t total_probes_ = 0;
    std::vector<Slot> table_;

    size_t hash_stride(uint64_t fingerprint) const {
        const uint64_t mixed = fingerprint ^ 0x9E3779B97F4A7C15ULL;
        return 1 + 2 * (mixed % (size_ >> 1));
    }

    void insert(std::string_view key, int32_t index) {
        const uint64_t fingerprint = packed_sparse_hash(key);
        size_t slot = fingerprint & mask_;
        const size_t stride = hash_stride(fingerprint);
        size_t probes = 0;
        while (table_[slot].base_index >= 0) {
            slot = (slot + stride) & mask_;
            if (++probes >= size_) throw std::runtime_error("packed HP-SFC table is full");
        }
        table_[slot] = Slot{fingerprint, index};
        total_probes_ += probes;
    }
};

class PackedPiecewiseModel {
public:
    struct Segment {
        double start_value;
        double slope;
        uint64_t anchor_index;
        uint64_t end_index;
    };

    void build(const MappedKeyStore& keys, int epsilon) {
        if (epsilon < 0) throw std::invalid_argument("epsilon must be nonnegative");
        if (keys.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("packed model exceeds int rank capacity");
        }
        epsilon_ = epsilon;
        segments_.clear();
        repaired_segments_ = 0;
        if (keys.size() == 0) return;
        size_t start = 0;
        double start_value = packed_key_to_double(keys.view(0));
        double previous_value = start_value;
        double lower = -std::numeric_limits<double>::infinity();
        double upper = std::numeric_limits<double>::infinity();

        for (size_t end = 1; end < keys.size(); ++end) {
            const double value = packed_key_to_double(keys.view(end));
            const double dx = value - start_value;
            bool violates = false;
            double next_lower = lower;
            double next_upper = upper;
            if (dx > 0.0) {
                next_lower = std::max(
                    lower, (static_cast<double>(end) - epsilon - start) / dx);
                next_upper = std::min(
                    upper, (static_cast<double>(end) + epsilon - start) / dx);
                violates = next_lower > next_upper;
            } else {
                violates = end - start > static_cast<size_t>(epsilon);
            }
            if (violates) {
                append_segment(keys, start, end - 1, start_value, previous_value, lower, upper);
                start = end;
                start_value = value;
                lower = -std::numeric_limits<double>::infinity();
                upper = std::numeric_limits<double>::infinity();
            } else {
                lower = next_lower;
                upper = next_upper;
            }
            previous_value = value;
        }
        append_segment(keys, start, keys.size() - 1, start_value, previous_value, lower, upper);
    }

    int predict(std::string_view key, const MappedKeyStore& keys) const {
        if (segments_.empty()) throw std::runtime_error("empty model has no prediction");
        const Segment& segment = segments_[segment_of(key, keys)];
        return predict_segment(segment, packed_key_to_double(key));
    }

    // Certification covers stored keys. For arbitrary boundaries, verify that the
    // recovered position brackets the query; otherwise use exact binary search.
    size_t lower_bound(std::string_view key, const MappedKeyStore& keys,
                       bool* used_fallback = nullptr) const {
        if (used_fallback) *used_fallback = false;
        if (keys.size() == 0) return 0;
        const int64_t predicted = predict(key, keys);
        size_t lo = static_cast<size_t>(std::max<int64_t>(0, predicted - epsilon_));
        size_t hi = static_cast<size_t>(std::min<int64_t>(keys.size(), predicted + epsilon_ + 1));
        lo = std::min(lo, keys.size());
        hi = std::max(lo, hi);
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (keys.view(mid).compare(key) < 0) lo = mid + 1;
            else hi = mid;
        }
        if ((lo == 0 || keys.view(lo - 1).compare(key) < 0) &&
            (lo == keys.size() || keys.view(lo).compare(key) >= 0)) return lo;
        if (used_fallback) *used_fallback = true;
        return keys.lower_bound(key);
    }

    size_t segment_of(std::string_view key, const MappedKeyStore& keys) const {
        size_t lo = 0;
        size_t hi = segments_.size() - 1;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (keys.view(segments_[mid].end_index).compare(key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    int max_error(const MappedKeyStore& keys) const {
        int maximum = 0;
        size_t segment_index = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            while (i > segments_[segment_index].end_index) ++segment_index;
            const Segment& segment = segments_[segment_index];
            const int prediction = predict_segment(segment, packed_key_to_double(keys.view(i)));
            maximum = std::max(maximum,
                std::abs(prediction - static_cast<int>(i)));
        }
        return maximum;
    }

    size_t size() const { return segments_.size(); }
    size_t repaired_segments() const { return repaired_segments_; }
    size_t memory_bytes() const { return segments_.capacity() * sizeof(Segment); }

private:
    int epsilon_ = 0;
    std::vector<Segment> segments_;
    size_t repaired_segments_ = 0;

    static int predict_segment(const Segment& segment, double value) {
        // Evaluate in local coordinates to avoid subtracting huge slope*x and
        // intercept values. Clamp before conversion, including out-of-domain keys.
        const double offset = segment.slope * (value - segment.start_value);
        const double rank = static_cast<double>(segment.anchor_index) + offset;
        if (rank <= 0.0) return 0;
        if (rank >= std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
        return static_cast<int>(std::llround(rank));
    }

    void append_segment(const MappedKeyStore& keys, size_t start, size_t end, double start_value,
                        double end_value, double lower, double upper) {
        double slope = 0.0;
        if (end > start && end_value > start_value) {
            if (std::isfinite(lower) && lower <= upper) slope = lower + (upper - lower) / 2.0;
            else slope = static_cast<double>(end - start) / (end_value - start_value);
        }
        slope = std::max(0.0, slope);
        const Segment candidate{start_value, slope, start, end};
        bool valid = std::isfinite(slope);
        for (size_t i = start; valid && i <= end; ++i) {
            valid = std::abs(predict_segment(candidate, packed_key_to_double(keys.view(i))) -
                             static_cast<int>(i)) <= epsilon_;
        }
        if (valid) {
            segments_.push_back(candidate);
            return;
        }
        // Numeric corner cases cannot silently invalidate a requested bound.
        // Exact constant blocks certify even if every approximate feature collides.
        ++repaired_segments_;
        for (size_t first = start; first <= end;) {
            const size_t last = first + std::min<size_t>(end - first, 2ULL * epsilon_);
            segments_.push_back(Segment{packed_key_to_double(keys.view(first)), 0.0,
                                       first + (last - first) / 2, last});
            first = last + 1;
        }
    }
};

class PackedRankTransportIndex {
public:
    PackedRankTransportIndex(const std::string& base_path, size_t expected_count,
                             int epsilon, bool build_fingerprints = true)
        : base_(base_path, expected_count), epsilon_(epsilon),
          has_fingerprints_(build_fingerprints) {
        if (!base_.strictly_sorted_unique()) {
            throw std::runtime_error("packed base keys are not strictly sorted and unique");
        }
        model_.build(base_, epsilon);
        if (has_fingerprints_) fingerprints_.build(base_);
        base_.advise_random();
    }

    bool insert(const std::string& key) {
        if (delta_.contains_inserted(key)) return false;
        if (base_.contains(key)) return delta_.discard_deleted(key);
        return delta_.mark_inserted(key);
    }

    bool remove(const std::string& key) {
        if (delta_.discard_inserted(key)) return true;
        if (!base_.contains(key) || delta_.contains_deleted(key)) return false;
        return delta_.mark_deleted(key);
    }

    bool point_lookup(const std::string& key) const {
        if (delta_.contains_inserted(key)) return true;
        if (delta_.contains_deleted(key)) return false;
        return has_fingerprints_ ? fingerprints_.lookup(key, base_) >= 0 : learned_point_lookup(key);
    }

    bool learned_point_lookup(const std::string& key) const {
        if (delta_.contains_inserted(key)) return true;
        if (delta_.contains_deleted(key)) return false;
        const size_t pos = model_.lower_bound(key, base_);
        return pos < base_.size() && base_.view(pos) == key;
    }

    int learned_exact_rank(const std::string& key) const {
        const size_t pos = model_.lower_bound(key, base_);
        if (delta_.contains_inserted(key) ||
            (pos < base_.size() && base_.view(pos) == key && !delta_.contains_deleted(key))) {
            return static_cast<int>(pos) + delta_.prefix_delta(key);
        }
        throw std::runtime_error("key not present in packed HRT-LI");
    }

    int learned_count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        const size_t left = model_.lower_bound(lo, base_);
        size_t right = model_.lower_bound(hi, base_);
        if (right < base_.size() && base_.view(right) == hi) ++right;
        return static_cast<int>(right - left) + delta_.prefix_delta_le(hi) - delta_.prefix_delta(lo);
    }

    size_t learned_base_lower_bound(const std::string& key, bool* used_fallback = nullptr) const {
        return model_.lower_bound(key, base_, used_fallback);
    }

    int exact_rank(const std::string& key) const {
        const size_t base_less = base_.lower_bound(key);
        if (delta_.contains_inserted(key)) {
            return static_cast<int>(base_less) + delta_.prefix_delta(key);
        }
        if (base_less < base_.size() && base_.view(base_less) == key &&
            !delta_.contains_deleted(key)) {
            return static_cast<int>(base_less) + delta_.prefix_delta(key);
        }
        throw std::runtime_error("key not present in packed HRT-LI");
    }

    int transported_predict(const std::string& key) const {
        if (!base_.contains(key) || delta_.contains_deleted(key)) {
            throw std::runtime_error("transported prediction requires a live base key");
        }
        return model_.predict(key, base_) + delta_.prefix_delta(key);
    }

    int count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        const int base_count = static_cast<int>(base_.upper_bound(hi) - base_.lower_bound(lo));
        return base_count + delta_.prefix_delta_le(hi) - delta_.prefix_delta(lo);
    }

    int base_model_max_error() const {
        return model_.max_error(base_);
    }

    const MappedKeyStore& base_store() const { return base_; }
    size_t model_segments() const { return model_.size(); }
    size_t repaired_model_segments() const { return model_.repaired_segments(); }
    size_t mutation_count() const {
        return static_cast<size_t>(delta_.mutation_count());
    }
    size_t fingerprint_capacity() const { return fingerprints_.capacity(); }
    double average_build_probes() const {
        return fingerprints_.average_build_probes(base_.size());
    }
    size_t estimated_memory_bytes() const {
        return base_.mapped_bytes() + base_.offsets_bytes() + model_.memory_bytes() +
               fingerprints_.memory_bytes() + delta_.memory_bytes();
    }

private:
    MappedKeyStore base_;
    int epsilon_;
    PackedPiecewiseModel model_;
    PackedFingerprintTable fingerprints_;
    bool has_fingerprints_;
    mutable PrefixRadixDelta delta_;
};

}  // namespace hrtli
