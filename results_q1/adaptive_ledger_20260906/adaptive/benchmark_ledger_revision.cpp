// Same natural signed state and independent sorted-prefix oracle for both ledgers.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include "prefix_radix_delta.hpp"

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        require(argc == 5, "usage: ledger_revision BASE_TXT ARRIVALS_NUL SEED VARIANT");
        uint64_t seed = std::stoull(argv[3]);
        std::vector<std::pair<std::string, int>> mutations;
        std::vector<std::string> probes;
        for (int which = 0; which < 2; ++which) {
            std::ifstream input(argv[which + 1], std::ios::binary);
            require(bool(input), "cannot read natural input");
            const size_t stride = which == 0 ? 80 : 20;
            size_t count = 0;
            std::string key;
            while (std::getline(input, key, which == 0 ? '\n' : '\0')) {
                require(!key.empty(), "empty natural key");
                if (count % stride == 0) {
                    mutations.emplace_back(key, which == 0 ? -1 : 1);
                    probes.push_back(key);
                } else if (count % stride == 1) probes.push_back(key);
                ++count;
            }
            require(count == (which == 0 ? 8000000 : 2000000), "natural input count mismatch");
        }
        require(mutations.size() == 200000 && probes.size() == 400000, "sample cardinality mismatch");
        auto sorted = mutations;
        std::sort(sorted.begin(), sorted.end());
        std::vector<int> cumulative(sorted.size() + 1);
        for (size_t i = 0; i < sorted.size(); ++i) {
            require(i == 0 || sorted[i - 1].first < sorted[i].first, "duplicate signed key");
            cumulative[i + 1] = cumulative[i] + sorted[i].second;
        }
        std::vector<int> expected(probes.size());
        for (size_t i = 0; i < probes.size(); ++i) {
            auto found = std::lower_bound(sorted.begin(), sorted.end(), probes[i],
                [](const auto& item, const auto& key) { return item.first < key; });
            expected[i] = cumulative[found - sorted.begin()];
        }
        std::mt19937_64 random(seed);
        std::shuffle(mutations.begin(), mutations.end(), random);
        std::vector<size_t> order(1048576);
        uint64_t hash = 14695981039346656037ULL;
        for (size_t& id : order) {
            id = random() % probes.size();
            for (unsigned shift = 0; shift < 64; shift += 8)
                hash = (hash ^ uint8_t(id >> shift)) * 1099511628211ULL;
        }
        std::vector<int> output(order.size());
        hrtli::PrefixRadixDelta delta;
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now();
        for (const auto& mutation : mutations)
            require(mutation.second == 1 ? delta.mark_inserted(mutation.first) : delta.mark_deleted(mutation.first), "mutation rejected");
        auto end = Clock::now();
        const double update_seconds = std::chrono::duration<double>(end - start).count();
        require(delta.validate_internal_lcps(), "ledger invariant failed");
        for (size_t i = 0; i < probes.size(); ++i)
            require(delta.prefix_delta(probes[i]) == expected[i], "pre-timing prefix oracle mismatch");
        start = Clock::now();
        for (size_t i = 0; i < order.size(); ++i) output[i] = delta.prefix_delta(probes[order[i]]);
        end = Clock::now();
        const double query_seconds = std::chrono::duration<double>(end - start).count();
        for (size_t i = 0; i < order.size(); ++i)
            require(output[i] == expected[order[i]], "timed prefix oracle mismatch");
        std::cout << std::setprecision(12) << "{\"variant\":\"" << argv[4] << "\",\"seed\":" << seed
            << ",\"natural_signed_keys\":" << mutations.size() << ",\"probe_pool\":" << probes.size()
            << ",\"queries\":" << order.size() << ",\"trace_fnv64\":\"" << hash << "\""
            << ",\"update_seconds\":" << update_seconds << ",\"prefix_seconds\":" << query_seconds
            << ",\"estimated_ledger_bytes\":" << delta.memory_bytes()
            << ",\"every_prefix_answer_checked\":true,\"internal_invariants_checked\":true}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
