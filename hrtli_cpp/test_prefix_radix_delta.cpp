#include <algorithm>
#include <cassert>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "prefix_radix_delta.hpp"

static int oracle_prefix(const std::map<std::string, int>& oracle,
                         const std::string& key, bool inclusive) {
    int sum = 0;
    for (const auto& entry : oracle) {
        if (entry.first < key || (inclusive && entry.first == key)) sum += entry.second;
        else break;
    }
    return sum;
}

int main() {
    hrtli::PrefixRadixDelta delta;
    std::map<std::string, int> oracle;
    const std::vector<std::string> keys = {
        "", "/a", "/a/b", "/a/beta", "/a/c", "/b", "/b/\x01", "/z"
    };

    std::mt19937 rng(20260814);
    std::uniform_int_distribution<int> key_pick(0, static_cast<int>(keys.size() - 1));
    std::uniform_int_distribution<int> op_pick(0, 3);
    for (int step = 0; step < 5000; ++step) {
        const std::string& key = keys[key_pick(rng)];
        int operation = op_pick(rng);
        int current = oracle.count(key) ? oracle[key] : 0;
        bool changed = false;
        if (operation == 0) {
            changed = delta.mark_inserted(key);
            if (current != 1) oracle[key] = 1;
            assert(changed == (current != 1));
        } else if (operation == 1) {
            changed = delta.mark_deleted(key);
            if (current != -1) oracle[key] = -1;
            assert(changed == (current != -1));
        } else if (operation == 2) {
            changed = delta.discard_inserted(key);
            if (current == 1) oracle.erase(key);
            assert(changed == (current == 1));
        } else {
            changed = delta.discard_deleted(key);
            if (current == -1) oracle.erase(key);
            assert(changed == (current == -1));
        }

        assert(delta.validate_internal_lcps());
        for (const auto& probe : keys) {
            assert(delta.prefix_delta(probe) == oracle_prefix(oracle, probe, false));
            assert(delta.prefix_delta_le(probe) == oracle_prefix(oracle, probe, true));
        }
    }

    std::vector<std::pair<std::string, int>> range;
    delta.iter_range("/a", "/b", range);
    assert(std::is_sorted(range.begin(), range.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; }));
    return 0;
}
