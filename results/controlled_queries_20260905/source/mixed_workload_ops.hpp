/**
 * Shared mixed-workload protocol used by the paper benchmark and the
 * independent audit driver.  Both programs generate the same operation
 * stream from (slug, write_ratio, trial, ops).
 *
 * Protocol:
 *   - Start from the loaded base snapshot.
 *   - Each of `ops` operations is a write with probability `write_ratio`,
 *     otherwise a lookup of base[i % |base|].
 *   - A write is a successful mutation: 25% delete a currently live key
 *     (if any exist), otherwise insert a currently absent key.
 *   - Insert keys come from the held-out insert file first.  After that
 *     file is exhausted, new unique keys are synthesized as
 *     base[j] + "/__mix_" + id so a 100% write mix never falls back to
 *     membership probes or no-op deletes.
 *   - The same pre-generated stream is executed by every index.
 */
#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

struct MixOp {
    enum Type : int { Lookup = 0, Insert = 1, Delete = 2 };
    Type type;
    std::string key;
};

struct MixStream {
    std::vector<MixOp> ops;
    int lookups = 0;
    int inserts = 0;
    int deletes = 0;
};

inline MixStream make_mixed_stream(const std::vector<std::string>& base,
                                   const std::vector<std::string>& inserts,
                                   double write_ratio,
                                   int trial,
                                   size_t ops) {
    MixStream stream;
    stream.ops.reserve(ops);

    std::vector<std::string> live = base;
    std::unordered_set<std::string> live_set(base.begin(), base.end());
    std::vector<std::string> absent;
    absent.reserve(inserts.size());
    for (const auto& key : inserts) {
        if (!live_set.count(key)) absent.push_back(key);
    }
    size_t absent_pos = 0;
    uint64_t synth = 0;

    std::mt19937 rng(20260815u + static_cast<unsigned>(trial) * 17u);
    std::uniform_real_distribution<double> uni(0.0, 1.0);

    auto take_absent = [&]() {
        if (absent_pos < absent.size()) return absent[absent_pos++];
        std::string key = base[synth % base.size()] + "/__mix_" + std::to_string(synth);
        ++synth;
        while (live_set.count(key)) {
            key = base[synth % base.size()] + "/__mix_" + std::to_string(synth);
            ++synth;
        }
        return key;
    };

    for (size_t i = 0; i < ops; ++i) {
        if (uni(rng) < write_ratio) {
            const bool want_delete = (stream.inserts + stream.deletes) % 4 == 0 && !live.empty();
            if (want_delete) {
                const size_t idx = (i + static_cast<size_t>(trial)) % live.size();
                std::string key = live[idx];
                live[idx] = std::move(live.back());
                live.pop_back();
                live_set.erase(key);
                stream.ops.push_back({MixOp::Delete, std::move(key)});
                ++stream.deletes;
            } else {
                std::string key = take_absent();
                live.push_back(key);
                live_set.insert(key);
                stream.ops.push_back({MixOp::Insert, std::move(key)});
                ++stream.inserts;
            }
        } else {
            stream.ops.push_back({MixOp::Lookup, base[i % base.size()]});
            ++stream.lookups;
        }
    }
    return stream;
}
