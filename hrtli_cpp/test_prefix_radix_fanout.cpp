#include <cassert>
#include <iostream>
#include <map>
#include <random>
#include "prefix_radix_delta.hpp"

int main() {
    hrtli::PrefixRadixDelta ledger;
    std::map<std::string, int> expected;
    std::vector<std::string> keys = {"", "shared", "shared/"};
    for (int byte = 0; byte < 256; ++byte) {
        keys.push_back(std::string(1, static_cast<char>(byte)));
        keys.push_back("shared/" + std::string(1, static_cast<char>(byte)) + "/leaf");
    }
    auto check = [&](const std::string& key) {
        int sum = 0;
        for (const auto& entry : expected) if (entry.first < key) sum += entry.second;
        const auto found = expected.find(key);
        const int weight = found == expected.end() ? 0 : found->second;
        assert(ledger.prefix_delta(key) == sum);
        assert(ledger.prefix_delta_le(key) == sum + weight);
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        const int sign = i % 3 ? 1 : -1;
        if (sign > 0) ledger.mark_inserted(keys[i]); else ledger.mark_deleted(keys[i]);
        expected[keys[i]] = sign;
        assert(ledger.validate_internal_lcps());
    }
    std::mt19937 rng(20260906);
    for (size_t step = 0; step < 4000; ++step) {
        const auto& key = keys[rng() % keys.size()];
        const auto found = expected.find(key);
        const int old = found == expected.end() ? 0 : found->second;
        const int next = int(rng() % 3) - 1;
        if (next == 1) assert(ledger.mark_inserted(key) == (old != 1));
        else if (next == -1) assert(ledger.mark_deleted(key) == (old != -1));
        else if (old == 1) assert(ledger.discard_inserted(key));
        else if (old == -1) assert(ledger.discard_deleted(key));
        if (next) expected[key] = next; else expected.erase(key);
        assert(ledger.validate_internal_lcps());
        for (int probe = 0; probe < 16; ++probe) check(keys[rng() % keys.size()]);
        if (step % 100 == 0) for (const auto& probe : keys) {
            check(probe);
            check(probe + std::string(1, '\0'));
        }
    }
    for (const auto& entry : expected) {
        if (entry.second > 0) assert(ledger.discard_inserted(entry.first));
        else assert(ledger.discard_deleted(entry.first));
        assert(ledger.validate_internal_lcps());
    }
    assert(ledger.empty() && ledger.prefix_delta("zzzz") == 0);
    std::cout << "PASS: 256-way fanout, all byte values, 4000 signed mutations, independent prefix oracle, complete cancellation\n";
}
