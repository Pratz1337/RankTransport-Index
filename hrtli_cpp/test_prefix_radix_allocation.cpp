// Fault injection for the actual mutation ledger, not benchmark measurements.
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include "prefix_radix_delta.hpp"

static long allocations_left = -1;
void* operator new(std::size_t bytes) {
    if (allocations_left == 0) throw std::bad_alloc();
    if (allocations_left > 0) --allocations_left;
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

static void check(const hrtli::PrefixRadixDelta& delta,
                  const std::map<std::string, int>& expected) {
    if (delta.size() != static_cast<int>(expected.size()) || !delta.validate_internal_lcps())
        throw std::runtime_error("failed allocation changed ledger size or subtree masses");
    std::vector<std::string> inserted, deleted;
    int prefix = 0;
    for (const auto& entry : expected) {
        if (delta.prefix_delta(entry.first) != prefix ||
            delta.prefix_delta_le(entry.first) != prefix + entry.second)
            throw std::runtime_error("prefix mass changed");
        if (delta.contains_inserted(entry.first) != (entry.second == 1) ||
            delta.contains_deleted(entry.first) != (entry.second == -1))
            throw std::runtime_error("terminal weight changed");
        (entry.second == 1 ? inserted : deleted).push_back(entry.first);
        prefix += entry.second;
    }
    if (delta.inserted_list() != inserted || delta.deleted_list() != deleted ||
        delta.inserted_size() != static_cast<int>(inserted.size()) ||
        delta.deleted_size() != static_cast<int>(deleted.size()))
        throw std::runtime_error("ledger counts or enumeration changed");
}

static void exercise(const std::map<std::string, int>& initial,
                     const std::string& key, int next_weight) {
    for (long fail_at = 0; fail_at < 32; ++fail_at) {
        hrtli::PrefixRadixDelta delta;
        for (const auto& entry : initial) {
            if (entry.second == 1) delta.mark_inserted(entry.first);
            else delta.mark_deleted(entry.first);
        }
        bool failed = false;
        allocations_left = fail_at;
        try {
            if (next_weight == 1) delta.mark_inserted(key);
            else if (next_weight == -1) delta.mark_deleted(key);
            else if (initial.at(key) == 1) delta.discard_inserted(key);
            else delta.discard_deleted(key);
        } catch (const std::bad_alloc&) { failed = true; }
        allocations_left = -1;
        auto expected = initial;
        if (!failed) {
            if (next_weight) expected[key] = next_weight;
            else expected.erase(key);
        }
        check(delta, expected);
        // Also prove the preserved structure still supports subsequent writes.
        delta.mark_inserted("/recovery");
        expected["/recovery"] = 1;
        check(delta, expected);
        if (!failed) return;
    }
    throw std::runtime_error("operation did not complete within allocation sweep");
}

int main() {
    try {
        const std::string prefix = "/" + std::string(80, 'p');
        exercise({}, prefix + "/first", 1);
        exercise({{prefix + "/first", 1}}, prefix + "/second", -1);
        exercise({{prefix + "/first", 1}}, prefix, -1);
        exercise({{prefix, -1}}, prefix + "/first", 1);
        exercise({{prefix, -1}, {prefix + "/first", 1}}, prefix, 0);
        exercise({{prefix + "/first", 1}, {prefix + "/second", -1}}, prefix + "/first", 0);
        exercise({{prefix, -1}}, prefix, 1);
        for (const std::string& stem : {std::string(), prefix}) {
            std::map<std::string, int> wide;
            for (int i = 0; i < 16; ++i) wide[stem + char('b' + i)] = i % 2 ? -1 : 1;
            exercise(wide, stem + "z", 1); // 16 -> 17 promotes a Fenwick cache.
            wide[stem + "z"] = 1;
            exercise(wide, stem + "z", 0); // 17 -> 16 removes the cache.
        }
        std::cout << "PASS: eleven mutation scenarios including cache promotion/demotion, allocation-failure sweeps and recovery writes\n";
    } catch (const std::exception& error) {
        allocations_left = -1;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
