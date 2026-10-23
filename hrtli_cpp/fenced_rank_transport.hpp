/**
 * FencedRankTransportIndex -- HRT-LI with a fence-aligned signed ledger.
 *
 * Contract (unchanged): S0 sorted unique base, unsigned-byte order,
 *   rank_t(k) = rank0(k) + Delta_t(k),  Delta_t(k) = sum_{x<k} w_t(x),
 *   w_t(x) = 1[x in S_t] - 1[x in S0].
 *
 * Mechanism.  The certified piecewise model already partitions key space by its
 * exact segment fences f_0 < f_1 < ... < f_{S-1} (the last base key of each
 * segment): bucket(k) = min{s : f_s >= k}, clamped to S-1.  Buckets are
 * contiguous key intervals, so for k in bucket b
 *   Delta_t(k) = sum_{s<b} mass(s) + sum_{x in bucket b, x<k} w_t(x).
 * One routed descent therefore serves base recovery, ledger membership and the
 * strict-prefix correction; a Fenwick tree over bucket masses supplies the
 * first term.  Small buckets are sorted vectors; a bucket that exceeds
 * kRadixThreshold entries is promoted to the signed radix ledger, so adversarial
 * concentration under one fence degrades to the previous per-bucket structure.
 *
 * Translation-invariant prediction.  p(k) = anchor_s + llround(slope_s * (v(k) -
 * start_s)).  Adding an integer D to anchor_s changes every prediction of the
 * segment by exactly D, with no floating-point re-rounding.  During
 * consolidation a segment whose bucket is empty has Delta_t constant (= D_s)
 * across its whole fence interval, so its keys' new ranks are old ranks + D_s and
 * |p'(k) - rank'(k)| = |p(k) - rank0(k)| <= epsilon: its certificate is
 * inherited exactly.  Only segments with ledger entries are refit and
 * exhaustively recertified.
 *
 * Serial; one consistent base/ledger state per call.  Consolidation builds the
 * complete next generation before publishing it: an exception leaves the old
 * generation and ledger intact (failure-atomic in memory, not crash-safe).
 * Ranks and masses are int64_t.  Base files exclude '\n' inside keys.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "prefix_radix_delta.hpp"

namespace hrtli {

// Newline-delimited sorted keys, either mapped from a file or owned in memory.
class BaseStore {
public:
    BaseStore() { offsets_.push_back(0); }

    static BaseStore map_file(const std::string& path, size_t expected_count) {
        BaseStore store;
        store.fd_ = ::open(path.c_str(), O_RDONLY);
        if (store.fd_ < 0) throw std::runtime_error("cannot open " + path);
        struct stat info {};
        if (::fstat(store.fd_, &info) != 0) throw std::runtime_error("cannot stat " + path);
        store.bytes_ = static_cast<size_t>(info.st_size);
        if (store.bytes_) {
            void* mapped = ::mmap(nullptr, store.bytes_, PROT_READ, MAP_PRIVATE, store.fd_, 0);
            if (mapped == MAP_FAILED) throw std::runtime_error("cannot mmap " + path);
            store.data_ = static_cast<const char*>(mapped);
            store.mapped_ = true;
        }
        store.offsets_.reserve(expected_count + 1);
        for (size_t i = 0; i < store.bytes_; ++i)
            if (store.data_[i] == '\n') store.offsets_.push_back(i + 1);
        if (store.offsets_.size() != expected_count + 1 || store.offsets_.back() != store.bytes_)
            throw std::runtime_error(path + " does not contain the expected newline-terminated key count");
        for (size_t i = 1; i < store.size(); ++i)
            if (store.view(i - 1).compare(store.view(i)) >= 0)
                throw std::runtime_error("base keys are not strictly sorted and unique");
        ::madvise(const_cast<char*>(store.data_), store.bytes_, MADV_RANDOM);
        return store;
    }

    static BaseStore own(std::vector<char>&& bytes, std::vector<uint64_t>&& offsets) {
        BaseStore store;
        store.owned_ = std::move(bytes);
        store.offsets_ = std::move(offsets);
        store.data_ = store.owned_.data();
        store.bytes_ = store.owned_.size();
        return store;
    }

    BaseStore(BaseStore&& other) noexcept { *this = std::move(other); }
    BaseStore& operator=(BaseStore&& other) noexcept {
        if (this != &other) {
            release();
            fd_ = std::exchange(other.fd_, -1);
            mapped_ = std::exchange(other.mapped_, false);
            bytes_ = std::exchange(other.bytes_, 0);
            owned_ = std::move(other.owned_);
            offsets_ = std::move(other.offsets_);
            data_ = mapped_ ? std::exchange(other.data_, nullptr) : owned_.data();
            other.data_ = nullptr;
            other.offsets_.assign(1, 0);
        }
        return *this;
    }
    BaseStore(const BaseStore&) = delete;
    BaseStore& operator=(const BaseStore&) = delete;
    ~BaseStore() { release(); }

    size_t size() const { return offsets_.size() - 1; }
    size_t key_bytes() const { return bytes_; }
    size_t offsets_bytes() const { return offsets_.capacity() * sizeof(uint64_t); }
    bool mapped() const { return mapped_; }
    const char* data() const { return data_; }
    uint64_t offset(size_t i) const { return offsets_[i]; }

    std::string_view view(size_t i) const {
        return std::string_view(data_ + offsets_[i], offsets_[i + 1] - offsets_[i] - 1);
    }

    size_t lower_bound(std::string_view key, size_t lo, size_t hi) const {
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (view(mid).compare(key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

private:
    int fd_ = -1;
    bool mapped_ = false;
    const char* data_ = nullptr;
    size_t bytes_ = 0;
    std::vector<char> owned_;
    std::vector<uint64_t> offsets_;

    void release() {
        if (mapped_ && data_) ::munmap(const_cast<char*>(data_), bytes_);
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        mapped_ = false;
        data_ = nullptr;
    }
};

inline double fenced_key_feature(std::string_view key) {
    // Same feature as the packed model: (byte+1) in base 1024, first 24 bytes.
    double value = 0.0;
    double denominator = 1024.0;
    const size_t limit = std::min<size_t>(key.size(), 24);
    for (size_t i = 0; i < limit; ++i) {
        value += static_cast<double>(static_cast<unsigned char>(key[i]) + 1U) / denominator;
        denominator *= 1024.0;
    }
    return value;
}

class FencedModel {
public:
    struct Segment {
        double start_value;
        double slope;
        int64_t anchor;      // integer offset added after local rounding
        uint64_t end_index;  // last base index covered (inclusive); its key is the fence
    };

    explicit FencedModel(int epsilon = 64) : epsilon_(epsilon) {
        if (epsilon < 0) throw std::invalid_argument("epsilon must be nonnegative");
    }

    int epsilon() const { return epsilon_; }
    size_t size() const { return segments_.size(); }
    size_t memory_bytes() const { return segments_.capacity() * sizeof(Segment); }
    const Segment& segment(size_t s) const { return segments_[s]; }
    size_t first_index(size_t s) const { return s == 0 ? 0 : segments_[s - 1].end_index + 1; }
    size_t repaired_segments() const { return repaired_; }

    void clear() { segments_.clear(); repaired_ = 0; }
    void reserve(size_t n) { segments_.reserve(n); }
    void push_shifted(const Segment& old, int64_t shift) {
        segments_.push_back(Segment{old.start_value, old.slope, old.anchor + shift,
                                    static_cast<uint64_t>(static_cast<int64_t>(old.end_index) + shift)});
    }

    // Greedy shrinking-cone fit of keys[first..last] (global indices), every
    // emitted segment exhaustively validated against the exact prediction formula.
    void fit_range(const BaseStore& keys, size_t first, size_t last) {
        if (first > last) return;
        size_t start = first;
        double start_value = fenced_key_feature(keys.view(first));
        double previous = start_value;
        double lower = -std::numeric_limits<double>::infinity();
        double upper = std::numeric_limits<double>::infinity();
        for (size_t end = first + 1; end <= last; ++end) {
            const double value = fenced_key_feature(keys.view(end));
            const double dx = value - start_value;
            bool violates;
            double next_lower = lower, next_upper = upper;
            const double offset = static_cast<double>(end - start);
            if (dx > 0.0) {
                next_lower = std::max(lower, (offset - epsilon_) / dx);
                next_upper = std::min(upper, (offset + epsilon_) / dx);
                violates = next_lower > next_upper;
            } else {
                violates = end - start > static_cast<size_t>(epsilon_);
            }
            if (violates) {
                append(keys, start, end - 1, start_value, previous, lower, upper);
                start = end;
                start_value = value;
                lower = -std::numeric_limits<double>::infinity();
                upper = std::numeric_limits<double>::infinity();
            } else {
                lower = next_lower;
                upper = next_upper;
            }
            previous = value;
        }
        append(keys, start, last, start_value, previous, lower, upper);
    }

    int64_t predict(size_t s, std::string_view key) const {
        return predict_segment(segments_[s], fenced_key_feature(key));
    }

    // Exact fence routing: first segment whose fence key is >= key.
    size_t route(std::string_view key, const BaseStore& keys) const {
        size_t lo = 0, hi = segments_.size() - 1;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (keys.view(segments_[mid].end_index).compare(key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // Exact base lower bound of key within routed segment s (certified window,
    // bracket check, exact-segment fallback; identical policy to the packed model).
    size_t lower_bound_in(size_t s, std::string_view key, const BaseStore& keys) const {
        const size_t n = keys.size();
        const size_t first = first_index(s);
        const size_t end = segments_[s].end_index + 1;
        const int64_t p = predict(s, key);
        size_t lo = static_cast<size_t>(std::clamp<int64_t>(p - epsilon_, first, end));
        size_t hi = static_cast<size_t>(std::clamp<int64_t>(p + epsilon_ + 1, first, end));
        lo = keys.lower_bound(key, lo, hi);
        if ((lo == 0 || keys.view(lo - 1).compare(key) < 0) && (lo == n || keys.view(lo).compare(key) >= 0))
            return lo;
        return keys.lower_bound(key, first, end);
    }

    int64_t max_error(const BaseStore& keys) const {
        int64_t worst = 0;
        size_t s = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            while (i > segments_[s].end_index) ++s;
            worst = std::max<int64_t>(worst, std::llabs(predict(s, keys.view(i)) - static_cast<int64_t>(i)));
        }
        return worst;
    }

private:
    int epsilon_;
    std::vector<Segment> segments_;
    size_t repaired_ = 0;

    static int64_t predict_segment(const Segment& seg, double value) {
        const double local = seg.slope * (value - seg.start_value);
        int64_t rounded;
        if (!(local > -9.0e15)) rounded = -static_cast<int64_t>(9e15);
        else if (!(local < 9.0e15)) rounded = static_cast<int64_t>(9e15);
        else rounded = std::llround(local);
        // Deliberately unclamped: certification and the exact anchor shift both
        // apply to this value; lower_bound_in clamps the search window instead.
        return seg.anchor + rounded;
    }

    void append(const BaseStore& keys, size_t start, size_t end, double start_value, double end_value,
                double lower, double upper) {
        double slope = 0.0;
        if (end > start && end_value > start_value) {
            if (std::isfinite(lower) && lower <= upper) slope = lower + (upper - lower) / 2.0;
            else slope = static_cast<double>(end - start) / (end_value - start_value);
        }
        slope = std::max(0.0, slope);
        const Segment candidate{start_value, slope, static_cast<int64_t>(start), end};
        bool valid = std::isfinite(slope);
        for (size_t i = start; valid && i <= end; ++i)
            valid = std::llabs(predict_segment(candidate, fenced_key_feature(keys.view(i))) -
                               static_cast<int64_t>(i)) <= epsilon_;
        if (valid) {
            segments_.push_back(candidate);
            return;
        }
        ++repaired_;
        for (size_t first = start; first <= end;) {
            const size_t last = first + std::min<size_t>(end - first, 2ULL * epsilon_);
            segments_.push_back(Segment{fenced_key_feature(keys.view(first)), 0.0,
                                        static_cast<int64_t>(first + (last - first) / 2), last});
            first = last + 1;
        }
    }
};

// Signed ledger partitioned by model fences.
class FencedLedger {
public:
    static constexpr size_t kRadixThreshold = 64;

    struct Probe {
        int weight;      // w_t(key): +1 inserted, -1 deleted base key, 0 otherwise
        int64_t before;  // sum of w_t(x) for x < key inside the bucket
    };

    void reset(size_t buckets) {
        buckets_.clear();
        buckets_.resize(buckets);
        mass_.assign(buckets, 0);
        block_fenwick_.assign((buckets + (kMassBlock - 1)) / kMassBlock + 1, 0);
        entries_ = inserted_ = deleted_ = 0;
        promoted_ = 0;
    }

    size_t entries() const { return entries_; }
    size_t inserted() const { return inserted_; }
    size_t deleted() const { return deleted_; }
    size_t promoted_buckets() const { return promoted_; }
    bool bucket_empty(size_t b) const { return buckets_[b].count == 0; }

    int64_t mass_before(size_t b) const {
        int64_t sum = 0;
        const size_t block = b / kMassBlock;
        for (size_t i = block; i; i -= i & (~i + 1)) sum += block_fenwick_[i];
        for (size_t i = block * kMassBlock; i < b; ++i) sum += mass_[i];
        return sum;
    }

    Probe probe(size_t b, const std::string& key) const {
        const Bucket& bucket = buckets_[b];
        if (bucket.count == 0) return {0, 0};
        if (bucket.radix) {
            return {bucket.radix->contains_inserted(key) ? 1 : bucket.radix->contains_deleted(key) ? -1 : 0,
                    bucket.radix->prefix_delta(key)};
        }
        int64_t before = 0;
        for (const Entry& e : bucket.small) {
            const int c = std::string_view(e.key).compare(key);
            if (c < 0) before += e.weight;
            else return {c == 0 ? e.weight : 0, before};
        }
        return {0, before};
    }

    // Precondition: current weight of key in bucket b is old_weight (from probe).
    void set(size_t b, const std::string& key, int old_weight, int new_weight) {
        if (old_weight == new_weight) return;
        Bucket& bucket = buckets_[b];
        if (!bucket.radix && old_weight == 0 && bucket.small.size() + 1 > kRadixThreshold) promote(bucket);
        if (bucket.radix) {
            bool ok;
            if (old_weight == 0) ok = new_weight > 0 ? bucket.radix->mark_inserted(key) : bucket.radix->mark_deleted(key);
            else ok = old_weight > 0 ? bucket.radix->discard_inserted(key) : bucket.radix->discard_deleted(key);
            if (!ok) throw std::logic_error("fenced ledger weight precondition violated");
        } else {
            auto it = std::lower_bound(bucket.small.begin(), bucket.small.end(), key,
                                       [](const Entry& e, const std::string& k) { return e.key < k; });
            if (old_weight == 0) bucket.small.insert(it, Entry{key, new_weight});
            else bucket.small.erase(it);
        }
        // Counters and masses change only after the structural edit succeeded.
        const int diff = new_weight - old_weight;
        bucket.count += old_weight == 0 ? 1 : -1;
        bucket.mass += diff;
        mass_[b] += static_cast<int32_t>(diff);
        const size_t block = b / kMassBlock;
        for (size_t i = block + 1; i < block_fenwick_.size(); i += i & (~i + 1)) block_fenwick_[i] += diff;
        entries_ += old_weight == 0 ? 1 : -1;
        if (old_weight > 0) --inserted_;
        if (old_weight < 0) --deleted_;
        if (new_weight > 0) ++inserted_;
        if (new_weight < 0) ++deleted_;
    }

    // Sorted (key, weight) entries of bucket b, for consolidation.
    template <class Visit>
    void for_each(size_t b, Visit visit) const {
        const Bucket& bucket = buckets_[b];
        if (bucket.count == 0) return;
        if (!bucket.radix) {
            for (const Entry& e : bucket.small) visit(std::string_view(e.key), e.weight);
            return;
        }
        auto ins = bucket.radix->inserted_list();
        auto del = bucket.radix->deleted_list();
        size_t i = 0, j = 0;
        while (i < ins.size() || j < del.size()) {
            if (j == del.size() || (i < ins.size() && ins[i] < del[j])) visit(std::string_view(ins[i++]), +1);
            else visit(std::string_view(del[j++]), -1);
        }
    }

    size_t memory_bytes() const {
        size_t total = buckets_.capacity() * sizeof(Bucket) + mass_.capacity() * sizeof(int32_t) +
                       block_fenwick_.capacity() * sizeof(int64_t);
        for (const Bucket& b : buckets_) {
            total += b.small.capacity() * sizeof(Entry);
            for (const Entry& e : b.small) if (e.key.capacity() > 15) total += e.key.capacity() + 1;
            if (b.radix) total += b.radix->memory_bytes();
        }
        return total;
    }

private:
    static constexpr size_t kMassBlock = 64;

    struct Entry {
        std::string key;
        int weight;
    };
    struct Bucket {
        std::vector<Entry> small;
        std::unique_ptr<PrefixRadixDelta> radix;
        int64_t count = 0;
        int64_t mass = 0;
    };

    std::vector<Bucket> buckets_;
    std::vector<int32_t> mass_;
    std::vector<int64_t> block_fenwick_;
    size_t entries_ = 0, inserted_ = 0, deleted_ = 0, promoted_ = 0;

    void promote(Bucket& bucket) {
        auto radix = std::make_unique<PrefixRadixDelta>();
        for (const Entry& e : bucket.small) {
            if (!(e.weight > 0 ? radix->mark_inserted(e.key) : radix->mark_deleted(e.key)))
                throw std::logic_error("duplicate entry in fenced bucket");
        }
        bucket.radix = std::move(radix);  // publish only after a complete copy
        std::vector<Entry>().swap(bucket.small);
        ++promoted_;
    }
};

// Optional exact search accelerators (rebuilt for every generation).
//  fence_prefix: 8-byte big-endian prefix of every fence key; routing compares
//    these first and falls back to a full string comparison only on equality.
//  key_slices: for every base key, the 8 bytes following its segment's longest
//    common prefix (LCP); window search compares slices first, then full keys on
//    equality.  Zero padding makes a strict slice inequality imply the same
//    strict lexicographic order, so both accelerators return identical results.
struct FencedOptions {
    bool fence_prefix = false;
    bool key_slices = false;
};

inline uint64_t slice8(std::string_view key, size_t offset) {
    uint64_t v = 0;
    if (offset + 8 <= key.size()) {
        std::memcpy(&v, key.data() + offset, 8);
        return __builtin_bswap64(v);
    }
    for (size_t i = 0; i < 8; ++i)
        v = (v << 8) | (offset + i < key.size() ? static_cast<unsigned char>(key[offset + i]) : 0U);
    return v;
}

struct ConsolidationStats {
    size_t old_keys = 0, new_keys = 0, old_segments = 0, new_segments = 0;
    size_t inherited_segments = 0, dirty_segments = 0, refit_keys = 0;
    size_t ledger_entries = 0, new_key_bytes = 0;
    double merge_copy_seconds = 0, refit_seconds = 0, accel_seconds = 0, total_seconds = 0;
    bool full_refit = false;
};

class FencedRankTransportIndex {
public:
    FencedRankTransportIndex(const std::string& base_path, size_t expected_count, int epsilon,
                             FencedOptions options = {})
        : base_(BaseStore::map_file(base_path, expected_count)), model_(epsilon), options_(options) {
        if (base_.size() == 0) throw std::invalid_argument("fenced index needs a nonempty base");
        model_.fit_range(base_, 0, base_.size() - 1);
        ledger_.reset(model_.size());
        build_accelerators(base_, model_, fence_prefix_, segment_lcp_, key_slices_);
    }

    bool contains(const std::string& key) const {
        const Locate at = locate(key);
        const FencedLedger::Probe p = ledger_.probe(at.segment, key);
        return p.weight > 0 || (p.weight == 0 && at.base_hit);
    }

    bool insert(const std::string& key) {
        const Locate at = locate(key);
        const FencedLedger::Probe p = ledger_.probe(at.segment, key);
        if (p.weight > 0) return false;
        if (at.base_hit) {
            if (p.weight == 0) return false;
            ledger_.set(at.segment, key, -1, 0);
            return true;
        }
        ledger_.set(at.segment, key, 0, +1);
        return true;
    }

    bool erase(const std::string& key) {
        const Locate at = locate(key);
        const FencedLedger::Probe p = ledger_.probe(at.segment, key);
        if (p.weight > 0) { ledger_.set(at.segment, key, +1, 0); return true; }
        if (p.weight == 0 && at.base_hit) { ledger_.set(at.segment, key, 0, -1); return true; }
        return false;
    }

    int64_t boundary_rank(const std::string& key) const {
        const Locate at = locate(key);
        return static_cast<int64_t>(at.base_pos) + ledger_.mass_before(at.segment) +
               ledger_.probe(at.segment, key).before;
    }

    // Live-key strict rank; rejects a non-live key.
    int64_t rank(const std::string& key) const {
        const Locate at = locate(key);
        const FencedLedger::Probe p = ledger_.probe(at.segment, key);
        if (!(p.weight > 0 || (p.weight == 0 && at.base_hit)))
            throw std::runtime_error("key not present in fenced HRT-LI");
        return static_cast<int64_t>(at.base_pos) + ledger_.mass_before(at.segment) + p.before;
    }

    int64_t count_range(const std::string& lo, const std::string& hi) const {
        if (lo > hi) return 0;
        const Locate a = locate(lo), b = locate(hi);
        const FencedLedger::Probe pa = ledger_.probe(a.segment, lo), pb = ledger_.probe(b.segment, hi);
        const bool hi_live = pb.weight > 0 || (pb.weight == 0 && b.base_hit);
        const int64_t right = static_cast<int64_t>(b.base_pos) + ledger_.mass_before(b.segment) + pb.before;
        const int64_t left = static_cast<int64_t>(a.base_pos) + ledger_.mass_before(a.segment) + pa.before;
        return right - left + (hi_live ? 1 : 0);
    }

    // Generation change.  full_refit=true refits every segment (control);
    // otherwise clean segments inherit their certificate by an exact shift.
    ConsolidationStats consolidate(bool full_refit = false) {
        using Clock = std::chrono::steady_clock;
        ConsolidationStats st;
        st.full_refit = full_refit;
        st.old_keys = base_.size();
        st.old_segments = model_.size();
        st.ledger_entries = ledger_.entries();
        const auto t0 = Clock::now();
        const size_t new_count = base_.size() + ledger_.inserted() - ledger_.deleted();
        if (new_count == 0) throw std::runtime_error("consolidation would empty the base");
        size_t extra_bytes = 0;
        for (size_t s = 0; s < model_.size(); ++s)
            ledger_.for_each(s, [&](std::string_view k, int w) { if (w > 0) extra_bytes += k.size() + 1; });
        std::vector<char> bytes;
        bytes.reserve(base_.key_bytes() + extra_bytes);
        std::vector<uint64_t> offsets;
        offsets.reserve(new_count + 1);
        offsets.push_back(0);
        struct Pending { size_t segment; size_t first, last; };
        std::vector<Pending> dirty;
        std::vector<uint8_t> clean(model_.size(), 0);
        auto emit = [&](std::string_view k) {
            bytes.insert(bytes.end(), k.begin(), k.end());
            bytes.push_back('\n');
            offsets.push_back(bytes.size());
        };
        for (size_t s = 0; s < model_.size(); ++s) {
            const size_t first = model_.first_index(s), last = model_.segment(s).end_index;
            if (!full_refit && ledger_.bucket_empty(s)) {
                // Bulk copy: offsets shift by the byte delta accumulated so far.
                const uint64_t src0 = base_.offset(first), src1 = base_.offset(last + 1);
                const int64_t byte_shift = static_cast<int64_t>(bytes.size()) - static_cast<int64_t>(src0);
                bytes.insert(bytes.end(), base_.data() + src0, base_.data() + src1);
                for (size_t i = first + 1; i <= last + 1; ++i)
                    offsets.push_back(static_cast<uint64_t>(static_cast<int64_t>(base_.offset(i)) + byte_shift));
                clean[s] = 1;
                continue;
            }
            const size_t out_first = offsets.size() - 1;
            size_t i = first;
            ledger_.for_each(s, [&](std::string_view k, int w) {
                while (i <= last && base_.view(i) < k) emit(base_.view(i++));
                if (w > 0) emit(k);
                else {
                    if (i > last || base_.view(i) != k) throw std::logic_error("deleted key missing from base");
                    ++i;
                }
            });
            while (i <= last) emit(base_.view(i++));
            const size_t out_last_plus = offsets.size() - 1;
            if (out_last_plus > out_first) dirty.push_back({s, out_first, out_last_plus - 1});
        }
        if (offsets.size() != new_count + 1) throw std::logic_error("consolidation key count mismatch");
        BaseStore next = BaseStore::own(std::move(bytes), std::move(offsets));
        const auto t1 = Clock::now();
        FencedModel next_model(model_.epsilon());
        next_model.reserve(model_.size() + dirty.size() * 2);
        size_t d = 0;
        int64_t running = 0;  // new index - old index for the next old segment
        for (size_t s = 0; s < model_.size(); ++s) {
            const size_t old_keys = model_.segment(s).end_index + 1 - model_.first_index(s);
            if (clean[s]) {
                next_model.push_shifted(model_.segment(s), running);
                ++st.inherited_segments;
                continue;
            }
            ++st.dirty_segments;
            size_t produced = 0;
            if (d < dirty.size() && dirty[d].segment == s) {
                next_model.fit_range(next, dirty[d].first, dirty[d].last);
                produced = dirty[d].last + 1 - dirty[d].first;
                st.refit_keys += produced;
                ++d;
            }
            running += static_cast<int64_t>(produced) - static_cast<int64_t>(old_keys);
        }
        const auto t2 = Clock::now();
        std::vector<uint64_t> next_prefix, next_slices;
        std::vector<uint32_t> next_lcp;
        build_accelerators(next, next_model, next_prefix, next_lcp, next_slices);
        const auto t3 = Clock::now();
        FencedLedger next_ledger;
        next_ledger.reset(next_model.size());
        // Publish: no further allocation can fail after this point.
        base_ = std::move(next);
        model_ = std::move(next_model);
        ledger_ = std::move(next_ledger);
        fence_prefix_.swap(next_prefix);
        segment_lcp_.swap(next_lcp);
        key_slices_.swap(next_slices);
        st.accel_seconds = std::chrono::duration<double>(t3 - t2).count();
        st.new_keys = base_.size();
        st.new_segments = model_.size();
        st.new_key_bytes = base_.key_bytes();
        st.merge_copy_seconds = std::chrono::duration<double>(t1 - t0).count();
        st.refit_seconds = std::chrono::duration<double>(t2 - t1).count();
        st.total_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        return st;
    }

    const BaseStore& base() const { return base_; }
    const FencedModel& model() const { return model_; }
    const FencedLedger& ledger() const { return ledger_; }
    int64_t certified_max_error() const { return model_.max_error(base_); }
    size_t accelerator_bytes() const {
        return fence_prefix_.capacity() * sizeof(uint64_t) + segment_lcp_.capacity() * sizeof(uint32_t) +
               key_slices_.capacity() * sizeof(uint64_t);
    }
    size_t memory_bytes_excluding_keys() const {
        return base_.offsets_bytes() + model_.memory_bytes() + ledger_.memory_bytes() + accelerator_bytes();
    }
    const FencedOptions& options() const { return options_; }

private:
    struct Locate {
        size_t segment;
        size_t base_pos;
        bool base_hit;
    };

    Locate locate(const std::string& key) const {
        const size_t s = options_.fence_prefix ? route_prefixed(key) : model_.route(key, base_);
        const size_t pos = options_.key_slices ? lower_bound_sliced(s, key) : model_.lower_bound_in(s, key, base_);
        return {s, pos, pos < base_.size() && base_.view(pos) == key};
    }

    size_t route_prefixed(std::string_view key) const {
        const uint64_t q = slice8(key, 0);
        size_t lo = 0, hi = fence_prefix_.size() - 1;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const uint64_t f = fence_prefix_[mid];
            const bool less = f != q ? f < q : base_.view(model_.segment(mid).end_index).compare(key) < 0;
            if (less) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    // Routing guarantees fence[s-1] < key and (key <= fence[s] or s is last), so
    // keys before the segment compare less and keys after it do not.
    size_t lower_bound_sliced(size_t s, std::string_view key) const {
        const size_t first = model_.first_index(s);
        const size_t end = model_.segment(s).end_index + 1;
        const uint32_t lcp = segment_lcp_[s];
        const std::string_view common = base_.view(first).substr(0, lcp);
        const int c = key.substr(0, std::min<size_t>(lcp, key.size())).compare(common);
        if (c < 0 || (c == 0 && key.size() < lcp)) return first;
        if (c > 0) return end;
        const uint64_t q = slice8(key, lcp);
        auto less = [&](size_t i) {
            if (i < first) return true;
            if (i >= end) return false;
            const uint64_t v = key_slices_[i];
            return v != q ? v < q : base_.view(i).compare(key) < 0;
        };
        const int64_t p = model_.predict(s, key);
        const int64_t eps = model_.epsilon();
        size_t lo = static_cast<size_t>(std::clamp<int64_t>(p - eps, first, end));
        size_t hi = static_cast<size_t>(std::clamp<int64_t>(p + eps + 1, first, end));
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (less(mid)) lo = mid + 1;
            else hi = mid;
        }
        if ((lo == 0 || less(lo - 1)) && !less(lo)) return lo;
        lo = first;
        hi = end;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (less(mid)) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }

    void build_accelerators(const BaseStore& base, const FencedModel& model, std::vector<uint64_t>& prefix,
                            std::vector<uint32_t>& lcp, std::vector<uint64_t>& slices) const {
        prefix.clear();
        lcp.clear();
        slices.clear();
        if (options_.fence_prefix) {
            prefix.reserve(model.size());
            for (size_t s = 0; s < model.size(); ++s) prefix.push_back(slice8(base.view(model.segment(s).end_index), 0));
        }
        if (options_.key_slices) {
            lcp.reserve(model.size());
            slices.reserve(base.size());
            for (size_t s = 0; s < model.size(); ++s) {
                const size_t first = model.first_index(s), last = model.segment(s).end_index;
                const std::string_view a = base.view(first), b = base.view(last);
                uint32_t l = 0;
                while (l < a.size() && l < b.size() && a[l] == b[l]) ++l;
                lcp.push_back(l);
                for (size_t i = first; i <= last; ++i) slices.push_back(slice8(base.view(i), l));
            }
        }
    }

    BaseStore base_;
    FencedModel model_;
    FencedLedger ledger_;
    FencedOptions options_;
    std::vector<uint64_t> fence_prefix_;
    std::vector<uint32_t> segment_lcp_;
    std::vector<uint64_t> key_slices_;
};

}  // namespace hrtli
