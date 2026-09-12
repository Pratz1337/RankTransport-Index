#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "counted_btree_delta.hpp"
#include "prefix_radix_delta.hpp"

struct Scenario {
    std::string name;
    std::vector<std::pair<std::string, int>> mutations;
    std::vector<std::string> probes;
};

struct Measurement {
    double update_ns;
    double prefix_ns;
    size_t memory_bytes;
};

static std::string pad(size_t value, int width) {
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << value;
    return out.str();
}

static std::vector<std::string> read_keys(const std::string& path, size_t limit) {
    std::ifstream input(path);
    std::vector<std::string> keys;
    std::string line;
    while (keys.size() < limit && std::getline(input, line)) {
        if (!line.empty()) keys.push_back(line);
    }
    return keys;
}

static Scenario real_scenario(const std::string& name) {
    Scenario scenario;
    scenario.name = name;
    auto inserted = read_keys("data/" + name + "_insert.txt", 50000);
    auto deleted = read_keys("data/" + name + "_initial.txt", inserted.size());
    for (const auto& key : inserted) scenario.mutations.emplace_back(key, +1);
    for (const auto& key : deleted) scenario.mutations.emplace_back(key, -1);
    scenario.probes = inserted;
    scenario.probes.insert(scenario.probes.end(), deleted.begin(), deleted.end());
    return scenario;
}

static Scenario shared_hierarchy(size_t count) {
    Scenario scenario;
    scenario.name = "shared_hierarchy_100k";
    scenario.mutations.reserve(count);
    scenario.probes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        std::string key = "/tenant/" + pad(i % 128, 3) + "/region/" +
                          pad((i / 128) % 16, 2) + "/service/" +
                          pad((i / 2048) % 32, 2) + "/resource/" + pad(i, 6);
        int weight = (i % 3 == 0) ? -1 : +1;
        scenario.mutations.emplace_back(key, weight);
        scenario.probes.push_back(key);
    }
    return scenario;
}

static Scenario shallow_keys(size_t count) {
    Scenario scenario;
    scenario.name = "shallow_100k";
    scenario.mutations.reserve(count);
    scenario.probes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        uint64_t mixed = (static_cast<uint64_t>(i) * 11400714819323198485ull);
        std::ostringstream out;
        out << std::hex << std::setw(16) << std::setfill('0') << mixed;
        std::string key = out.str();
        int weight = (i % 3 == 0) ? -1 : +1;
        scenario.mutations.emplace_back(key, weight);
        scenario.probes.push_back(key);
    }
    return scenario;
}

template <typename Delta>
static void populate(Delta& delta, const Scenario& scenario) {
    for (const auto& mutation : scenario.mutations) {
        bool changed = mutation.second > 0 ? delta.mark_inserted(mutation.first)
                                           : delta.mark_deleted(mutation.first);
        if (!changed) {
            std::cerr << "duplicate mutation key in " << scenario.name << '\n';
            std::exit(2);
        }
    }
}

static double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

template <typename Delta>
static Measurement measure(const Scenario& scenario, int trials, size_t query_count) {
    std::vector<double> update_times;
    std::vector<double> query_times;
    size_t memory = 0;
    volatile int sink = 0;

    for (int trial = 0; trial < trials; ++trial) {
        Delta delta;
        auto update_start = std::chrono::steady_clock::now();
        populate(delta, scenario);
        auto update_end = std::chrono::steady_clock::now();
        if (!delta.validate_internal_lcps()) std::exit(3);
        update_times.push_back(std::chrono::duration<double, std::nano>(update_end - update_start).count() /
                               static_cast<double>(scenario.mutations.size()));
        memory = delta.memory_bytes();

        std::mt19937 rng(1000 + trial);
        std::uniform_int_distribution<size_t> pick(0, scenario.probes.size() - 1);
        std::vector<size_t> order(query_count);
        for (size_t& index : order) index = pick(rng);
        auto query_start = std::chrono::steady_clock::now();
        for (size_t index : order) sink += delta.prefix_delta(scenario.probes[index]);
        auto query_end = std::chrono::steady_clock::now();
        query_times.push_back(std::chrono::duration<double, std::nano>(query_end - query_start).count() /
                              static_cast<double>(query_count));
    }
    if (sink == 123456789) std::cerr << sink;
    return {median(update_times), median(query_times), memory};
}

static void verify_equivalence(const Scenario& scenario) {
    hrtli::CountedBTreeDelta flat;
    hrtli::PrefixRadixDelta radix;
    populate(flat, scenario);
    populate(radix, scenario);
    for (const auto& probe : scenario.probes) {
        if (flat.prefix_delta(probe) != radix.prefix_delta(probe) ||
            flat.prefix_delta_le(probe) != radix.prefix_delta_le(probe)) {
            std::cerr << "prefix oracle mismatch for " << scenario.name << '\n';
            std::exit(4);
        }
    }
}

int main() {
    std::vector<Scenario> scenarios;
    scenarios.push_back(real_scenario("dns"));
    scenarios.push_back(real_scenario("json"));
    scenarios.push_back(shared_hierarchy(100000));
    scenarios.push_back(shallow_keys(100000));

    std::ofstream json("results_q1/prefix_radix_delta.json");
    json << "{\n  \"trials\": 7,\n  \"query_count_per_trial\": 500000,\n  \"scenarios\": [\n";
    for (size_t i = 0; i < scenarios.size(); ++i) {
        const Scenario& scenario = scenarios[i];
        verify_equivalence(scenario);
        Measurement flat = measure<hrtli::CountedBTreeDelta>(scenario, 7, 500000);
        Measurement radix = measure<hrtli::PrefixRadixDelta>(scenario, 7, 500000);
        double query_speedup = flat.prefix_ns / radix.prefix_ns;
        double update_speedup = flat.update_ns / radix.update_ns;
        double memory_ratio = static_cast<double>(radix.memory_bytes) / flat.memory_bytes;

        std::cout << scenario.name << " m=" << scenario.mutations.size()
                  << " prefix(ns) flat=" << flat.prefix_ns << " radix=" << radix.prefix_ns
                  << " speedup=" << query_speedup
                  << " memory_ratio=" << memory_ratio << '\n';
        json << "    {\n"
             << "      \"name\": \"" << scenario.name << "\",\n"
             << "      \"mutations\": " << scenario.mutations.size() << ",\n"
             << "      \"flat_update_ns\": " << flat.update_ns << ",\n"
             << "      \"radix_update_ns\": " << radix.update_ns << ",\n"
             << "      \"flat_prefix_ns\": " << flat.prefix_ns << ",\n"
             << "      \"radix_prefix_ns\": " << radix.prefix_ns << ",\n"
             << "      \"flat_memory_bytes\": " << flat.memory_bytes << ",\n"
             << "      \"radix_memory_bytes\": " << radix.memory_bytes << ",\n"
             << "      \"prefix_speedup\": " << query_speedup << ",\n"
             << "      \"update_speedup\": " << update_speedup << ",\n"
             << "      \"memory_ratio\": " << memory_ratio << "\n"
             << "    }" << (i + 1 == scenarios.size() ? "" : ",") << "\n";
    }
    json << "  ]\n}\n";
    return 0;
}
