#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "counted_radix_ost.hpp"
#include "mixed_workload_ops.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::steady_clock;

static std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream input(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(input, line)) if (!line.empty()) keys.push_back(line);
    return keys;
}

static std::vector<std::string> unique_sorted(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

static double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

int main() {
    struct Spec { const char* name; const char* slug; } specs[] = {
        {"Filesystem paths", "filesystem"},
        {"DNS hierarchy", "dns"},
        {"JSON paths", "json"},
    };
    const double ratios[] = {0.0, 0.05, 0.25, 0.50, 0.75, 1.0};
    const int trials = 30;
    const size_t ops = 20000;

    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n  \"driver\": \"audit_mixed_workload\",\n"
         << "  \"trials\": " << trials << ",\n"
         << "  \"operations_per_trial\": " << ops << ",\n"
         << "  \"lookup_results_consumed\": true,\n  \"rows\": [\n";
    bool first = true;
    for (const auto& spec : specs) {
        auto base = unique_sorted(read_keys(std::string("data/") + spec.slug + "_initial.txt"));
        auto raw_inserts = read_keys(std::string("data/") + spec.slug + "_insert.txt");
        std::unordered_set<std::string> present(base.begin(), base.end());
        std::vector<std::string> inserts;
        for (const auto& key : raw_inserts) if (!present.count(key)) inserts.push_back(key);

        for (double ratio : ratios) {
            std::vector<double> hrt_ns, ost_ns;
            int lookups = 0, inserts_n = 0, deletes = 0;
            int hrt_hits = 0, ost_hits = 0, hrt_writes = 0, ost_writes = 0;
            for (int trial = 0; trial < trials; ++trial) {
                MixStream stream = make_mixed_stream(base, inserts, ratio, trial, ops);
                lookups = stream.lookups;
                inserts_n = stream.inserts;
                deletes = stream.deletes;
                hrtli::RankTransportIndex hrt(base, 64);
                hrtli::CountedRadixOST ost;
                ost.bulk_load(base);
                int trial_hrt_hits = 0, trial_ost_hits = 0;
                int trial_hrt_writes = 0, trial_ost_writes = 0;
                uint64_t hrt_sink = 0, ost_sink = 0;
                auto t0 = Clock::now();
                for (const auto& op : stream.ops) {
                    if (op.type == MixOp::Lookup) {
                        const bool found = hrt.point_lookup(op.key);
                        trial_hrt_hits += int(found);
                        hrt_sink = hrt_sink * 131U + uint64_t(found);
                    }
                    else if (op.type == MixOp::Insert) trial_hrt_writes += int(hrt.insert(op.key));
                    else trial_hrt_writes += int(hrt.remove(op.key));
                }
                auto t1 = Clock::now();
                auto t2 = Clock::now();
                for (const auto& op : stream.ops) {
                    if (op.type == MixOp::Lookup) {
                        const bool found = ost.point_lookup(op.key);
                        trial_ost_hits += int(found);
                        ost_sink = ost_sink * 131U + uint64_t(found);
                    }
                    else if (op.type == MixOp::Insert) trial_ost_writes += int(ost.insert(op.key));
                    else trial_ost_writes += int(ost.remove(op.key));
                }
                auto t3 = Clock::now();
                const int expected_writes = stream.inserts + stream.deletes;
                if (trial_hrt_writes != expected_writes ||
                    trial_ost_writes != expected_writes ||
                    trial_hrt_hits != trial_ost_hits || hrt_sink != ost_sink) {
                    throw std::runtime_error(
                        std::string("mixed-workload correctness gate failed: ") +
                        spec.name + " ratio=" + std::to_string(ratio) +
                        " trial=" + std::to_string(trial) +
                        " hrt_hits=" + std::to_string(trial_hrt_hits) +
                        " ost_hits=" + std::to_string(trial_ost_hits));
                }
                hrt_hits = trial_hrt_hits;
                ost_hits = trial_ost_hits;
                hrt_writes = trial_hrt_writes;
                ost_writes = trial_ost_writes;
                hrt_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / ops);
                ost_ns.push_back(std::chrono::duration<double, std::nano>(t3 - t2).count() / ops);
            }
            if (!first) json << ",\n";
            first = false;
            json << "    {\"workload\": \"" << spec.name << "\"";
            json << ", \"write_ratio\": " << ratio;
            json << ", \"lookups\": " << lookups;
            json << ", \"inserts\": " << inserts_n;
            json << ", \"deletes\": " << deletes;
            json << ", \"hrtli_lookup_hits\": " << hrt_hits;
            json << ", \"ost_lookup_hits\": " << ost_hits;
            json << ", \"hrtli_write_successes\": " << hrt_writes;
            json << ", \"ost_write_successes\": " << ost_writes;
            json << ", \"hrtli_ns_median\": " << median(hrt_ns);
            json << ", \"ost_ns_median\": " << median(ost_ns) << "}";
        }
    }
    json << "\n  ]\n}\n";
    std::ofstream out("results_q1/mixed_workload_audit.json");
    out << json.str();
    std::cout << json.str();
    return 0;
}
