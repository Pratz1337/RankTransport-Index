#pragma once

#include <string>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cmath>

namespace hrtli {

#ifndef HRTLI_KEY_TO_DOUBLE_DEFINED
#define HRTLI_KEY_TO_DOUBLE_DEFINED
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

class ApproxLocalDelta {
private:
    double start_;
    double end_;
    int num_bins_;
    int res_limit_;

    std::unordered_set<std::string> inserted_set_;
    std::unordered_set<std::string> deleted_set_;

    std::vector<int> bins_;
    std::vector<std::pair<std::string, int>> residual_buffer_;

    int get_bin_idx(double v) const {
        if (end_ <= start_) return 0;
        int idx = static_cast<int>((v - start_) / ((end_ - start_) / num_bins_));
        if (idx < 0) return 0;
        if (idx >= num_bins_) return num_bins_ - 1;
        return idx;
    }

public:
    ApproxLocalDelta(double start = 0.0, double end = 1.0, int num_bins = 16, int res_limit = 8)
        : start_(start), end_(end), num_bins_(num_bins > 0 ? num_bins : 1), res_limit_(res_limit),
          bins_(num_bins > 0 ? num_bins : 1, 0) {}

    ~ApproxLocalDelta() = default;

    ApproxLocalDelta(const ApproxLocalDelta&) = default;
    ApproxLocalDelta& operator=(const ApproxLocalDelta&) = default;
    ApproxLocalDelta(ApproxLocalDelta&&) noexcept = default;
    ApproxLocalDelta& operator=(ApproxLocalDelta&&) noexcept = default;

    void reset() {
        inserted_set_.clear();
        deleted_set_.clear();
        std::fill(bins_.begin(), bins_.end(), 0);
        residual_buffer_.clear();
    }

    int size() const {
        return static_cast<int>(inserted_set_.size() + deleted_set_.size());
    }

    int mutation_count() const {
        return size();
    }

    int inserted_size() const {
        return static_cast<int>(inserted_set_.size());
    }

    int deleted_size() const {
        return static_cast<int>(deleted_set_.size());
    }

    bool empty() const {
        return inserted_set_.empty() && deleted_set_.empty();
    }

    size_t memory_bytes() const {
        size_t total = sizeof(*this);
        total += inserted_set_.bucket_count() * sizeof(void*);
        for (const auto& k : inserted_set_) {
            total += sizeof(std::string) + k.capacity();
        }
        total += deleted_set_.bucket_count() * sizeof(void*);
        for (const auto& k : deleted_set_) {
            total += sizeof(std::string) + k.capacity();
        }
        total += bins_.capacity() * sizeof(int);
        total += residual_buffer_.capacity() * sizeof(std::pair<std::string, int>);
        for (const auto& entry : residual_buffer_) {
            total += entry.first.capacity();
        }
        return total;
    }

    bool contains_inserted(const std::string& key) const {
        return inserted_set_.find(key) != inserted_set_.end();
    }

    bool contains_deleted(const std::string& key) const {
        return deleted_set_.find(key) != deleted_set_.end();
    }

    bool mark_inserted(const std::string& key) {
        if (contains_inserted(key)) return false;
        inserted_set_.insert(key);

        auto it = std::find_if(residual_buffer_.begin(), residual_buffer_.end(),
                               [&key](const auto& pair) { return pair.first == key; });
        if (it != residual_buffer_.end()) {
            it->second = 1;
        } else {
            if (residual_buffer_.size() < static_cast<size_t>(res_limit_)) {
                residual_buffer_.push_back({key, 1});
            } else {
                int bin_idx = get_bin_idx(hli_key_to_double(key));
                bins_[bin_idx] += 1;
            }
        }
        return true;
    }

    bool mark_deleted(const std::string& key) {
        if (contains_deleted(key)) return false;
        deleted_set_.insert(key);

        auto it = std::find_if(residual_buffer_.begin(), residual_buffer_.end(),
                               [&key](const auto& pair) { return pair.first == key; });
        if (it != residual_buffer_.end()) {
            it->second = -1;
        } else {
            if (residual_buffer_.size() < static_cast<size_t>(res_limit_)) {
                residual_buffer_.push_back({key, -1});
            } else {
                int bin_idx = get_bin_idx(hli_key_to_double(key));
                bins_[bin_idx] -= 1;
            }
        }
        return true;
    }

    bool discard_inserted(const std::string& key) {
        auto set_it = inserted_set_.find(key);
        if (set_it == inserted_set_.end()) return false;
        inserted_set_.erase(set_it);

        auto it = std::find_if(residual_buffer_.begin(), residual_buffer_.end(),
                               [&key](const auto& pair) { return pair.first == key; });
        if (it != residual_buffer_.end()) {
            residual_buffer_.erase(it);
        } else {
            int bin_idx = get_bin_idx(hli_key_to_double(key));
            bins_[bin_idx] -= 1;
        }
        return true;
    }

    bool discard_deleted(const std::string& key) {
        auto set_it = deleted_set_.find(key);
        if (set_it == deleted_set_.end()) return false;
        deleted_set_.erase(set_it);

        auto it = std::find_if(residual_buffer_.begin(), residual_buffer_.end(),
                               [&key](const auto& pair) { return pair.first == key; });
        if (it != residual_buffer_.end()) {
            residual_buffer_.erase(it);
        } else {
            int bin_idx = get_bin_idx(hli_key_to_double(key));
            bins_[bin_idx] += 1;
        }
        return true;
    }

    int prefix_delta(const std::string& key) const {
        double v = hli_key_to_double(key);
        double sum = 0.0;

        for (const auto& entry : residual_buffer_) {
            if (entry.first < key) {
                sum += entry.second;
            }
        }

        if (num_bins_ > 0) {
            int key_bin = get_bin_idx(v);
            for (int i = 0; i < key_bin; ++i) {
                sum += bins_[i];
            }
            double bin_width = (end_ > start_) ? (end_ - start_) / num_bins_ : 1.0;
            double bin_start = start_ + key_bin * bin_width;
            double frac = 0.0;
            if (bin_width > 0.0) {
                frac = (v - bin_start) / bin_width;
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
            }
            sum += frac * bins_[key_bin];
        }

        return static_cast<int>(std::round(sum));
    }

    int prefix_delta_le(const std::string& key) const {
        double v = hli_key_to_double(key);
        double sum = 0.0;

        for (const auto& entry : residual_buffer_) {
            if (entry.first <= key) {
                sum += entry.second;
            }
        }

        if (num_bins_ > 0) {
            int key_bin = get_bin_idx(v);
            for (int i = 0; i < key_bin; ++i) {
                sum += bins_[i];
            }
            double bin_width = (end_ > start_) ? (end_ - start_) / num_bins_ : 1.0;
            double bin_start = start_ + key_bin * bin_width;
            double frac = 0.0;
            if (bin_width > 0.0) {
                frac = (v - bin_start) / bin_width;
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
            }
            sum += frac * bins_[key_bin];
        }

        return static_cast<int>(std::round(sum));
    }

    void iter_range(const std::string& lo, const std::string& hi,
                    std::vector<std::pair<std::string, int>>& out) const {
        std::vector<std::pair<std::string, int>> temp;
        for (const auto& key : inserted_set_) {
            if (key >= lo && key <= hi) {
                temp.push_back({key, 1});
            }
        }
        for (const auto& key : deleted_set_) {
            if (key >= lo && key <= hi) {
                temp.push_back({key, -1});
            }
        }
        std::sort(temp.begin(), temp.end());
        out.insert(out.end(), temp.begin(), temp.end());
    }

    std::vector<std::string> inserted_list() const {
        std::vector<std::string> out(inserted_set_.begin(), inserted_set_.end());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> deleted_list() const {
        std::vector<std::string> out(deleted_set_.begin(), deleted_set_.end());
        std::sort(out.begin(), out.end());
        return out;
    }
};

} // namespace hrtli
