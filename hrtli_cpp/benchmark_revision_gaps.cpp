#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "counted_radix_ost.hpp"
#include "mixed_workload_ops.hpp"
#include "rank_transport.hpp"
#include "treap.hpp"

using Clock = std::chrono::steady_clock;

static std::vector<std::string> read_keys(const std::string& path, size_t limit = 0) {
    std::ifstream input(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        keys.push_back(line);
        if (limit && keys.size() >= limit) break;
    }
    return keys;
}

static std::vector<std::string> unique_sorted(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

static double ns_per_op(Clock::time_point start, Clock::time_point end, size_t ops) {
    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    return ops ? ns / static_cast<double>(ops) : 0.0;
}

static double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

static std::pair<double, double> percentile_ci(std::vector<double> values) {
    if (values.empty()) return {0.0, 0.0};
    std::sort(values.begin(), values.end());
    auto at = [&](double q) {
        size_t idx = static_cast<size_t>(std::llround(q * (values.size() - 1)));
        return values[std::min(idx, values.size() - 1)];
    };
    return {at(0.025), at(0.975)};
}

static void json_escape(std::ostream& out, const std::string& text) {
    out << '"';
    for (char c : text) {
        if (c == '\\' || c == '"') out << '\\' << c;
        else out << c;
    }
    out << '"';
}

struct Workload {
    std::string name;
    std::string slug;
    std::vector<std::string> base;
    std::vector<std::string> inserts;
};

static Workload load_workload(const std::string& name, const std::string& slug) {
    Workload w;
    w.name = name;
    w.slug = slug;
    w.base = unique_sorted(read_keys("data/" + slug + "_initial.txt"));
    auto raw_inserts = read_keys("data/" + slug + "_insert.txt");
    std::unordered_set<std::string> present(w.base.begin(), w.base.end());
    for (const auto& key : raw_inserts) {
        if (!present.count(key)) {
            w.inserts.push_back(key);
            present.insert(key);
        }
    }
    return w;
}

static int max_base_error(const hrtli::RankTransportIndex& idx, const std::vector<std::string>& keys) {
    int worst = 0;
    for (const auto& key : keys) {
        if (!idx.contains(key) || !idx.contains_base(key)) continue;
        int err = std::abs(idx.transported_predict(key) - idx.exact_rank(key));
        worst = std::max(worst, err);
    }
    return worst;
}

static int inserted_rank_errors(const hrtli::RankTransportIndex& idx, const std::vector<std::string>& keys) {
    int errors = 0;
    for (const auto& key : keys) {
        if (!idx.contains(key) || idx.contains_base(key)) continue;
        if (idx.lookup(key) != idx.exact_rank(key)) ++errors;
    }
    return errors;
}

int main() {
    const int trials = 7;
    const int epsilons[] = {16, 32, 64, 128};
    const double write_ratios[] = {0.0, 0.05, 0.25, 0.50, 0.75, 1.0};
    std::vector<Workload> workloads = {
        load_workload("Filesystem paths", "filesystem"),
        load_workload("DNS hierarchy", "dns"),
        load_workload("JSON paths", "json"),
    };

    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n  \"platform\": \"WSL2\",\n";
    json << "  \"compiler\": \"g++ -O3 -std=c++17\",\n";
    json << "  \"cpu\": \"13th Gen Intel Core i5-13500H\",\n";
    json << "  \"notes\": \"Mixed stream uses shared make_mixed_stream: successful inserts/deletes only, same ops for both indexes. Existing paper tables were not overwritten.\",\n";

    json << "  \"epsilon_sensitivity\": [\n";
    bool first = true;
    for (const auto& w : workloads) {
        for (int eps : epsilons) {
            hrtli::RankTransportIndex idx(w.base, eps);
            size_t n_ins = std::min(w.inserts.size(), w.base.size() / 5 + 8);
            size_t n_del = std::min(w.base.size() / 10, static_cast<size_t>(32));
            for (size_t i = 0; i < n_ins; ++i) idx.insert(w.inserts[i]);
            for (size_t i = 0; i < n_del; ++i) idx.remove(w.base[i * 3 % w.base.size()]);
            int certified = idx.get_model_max_error();
            int live = max_base_error(idx, w.base);
            int inserted_errors = inserted_rank_errors(idx, w.inserts);
            if (!first) json << ",\n";
            first = false;
            json << "    {\"workload\": "; json_escape(json, w.name);
            json << ", \"epsilon\": " << eps;
            json << ", \"segments\": " << idx.model_segments_count();
            json << ", \"certified_max_error\": " << certified;
            json << ", \"live_max_error\": " << live;
            json << ", \"inserted_rank_errors\": " << inserted_errors;
            json << ", \"preserved\": " << (live <= eps && inserted_errors == 0 ? "true" : "false") << "}";
        }
    }
    json << "\n  ],\n";

    json << "  \"exact_rank_baseline\": [\n";
    first = true;
    for (const auto& w : workloads) {
        std::vector<double> hrt_rank, ost_rank, treap_rank, hrt_count, ost_count;
        std::vector<double> hrt_ins, ost_ins, hrt_del, ost_del;
        size_t mem_hrt = 0, mem_ost = 0, mem_treap = 0;
        size_t n_ins = std::min(w.inserts.size(), static_cast<size_t>(256));
        size_t n_del = std::min(w.base.size() / 8, static_cast<size_t>(64));
        std::vector<std::string> probes = w.base;
        if (probes.size() > 400) probes.resize(400);
        for (int trial = 0; trial < trials; ++trial) {
            hrtli::RankTransportIndex hrt(w.base, 64);
            hrtli::CountedRadixOST ost;
            hrtli::OrderStatisticTreap treap;
            ost.bulk_load(w.base);
            for (const auto& key : w.base) treap.add(key);

            auto t0 = Clock::now();
            for (size_t i = 0; i < n_ins; ++i) hrt.insert(w.inserts[i]);
            auto t1 = Clock::now();
            for (size_t i = 0; i < n_ins; ++i) ost.insert(w.inserts[i]);
            auto t2 = Clock::now();
            hrt_ins.push_back(ns_per_op(t0, t1, n_ins));
            ost_ins.push_back(ns_per_op(t1, t2, n_ins));

            t0 = Clock::now();
            for (size_t i = 0; i < n_del; ++i) hrt.remove(w.base[(i * 5 + trial) % w.base.size()]);
            t1 = Clock::now();
            for (size_t i = 0; i < n_del; ++i) ost.remove(w.base[(i * 5 + trial) % w.base.size()]);
            t2 = Clock::now();
            hrt_del.push_back(ns_per_op(t0, t1, n_del));
            ost_del.push_back(ns_per_op(t1, t2, n_del));

            std::vector<std::string> live_probes;
            for (const auto& key : probes) if (hrt.contains(key)) live_probes.push_back(key);
            if (live_probes.empty()) live_probes.push_back(w.base.front());

            t0 = Clock::now();
            volatile long long sink = 0;
            for (int rep = 0; rep < 8; ++rep) {
                for (const auto& key : live_probes) sink += hrt.exact_rank(key);
            }
            t1 = Clock::now();
            for (int rep = 0; rep < 8; ++rep) {
                for (const auto& key : live_probes) sink += ost.rank(key);
            }
            t2 = Clock::now();
            Clock::time_point t3 = t2;
            for (int rep = 0; rep < 8; ++rep) {
                for (const auto& key : live_probes) sink += treap.rank(key);
            }
            t3 = Clock::now();
            (void)sink;
            hrt_rank.push_back(ns_per_op(t0, t1, 8 * live_probes.size()));
            ost_rank.push_back(ns_per_op(t1, t2, 8 * live_probes.size()));
            treap_rank.push_back(ns_per_op(t2, t3, 8 * live_probes.size()));

            t0 = Clock::now();
            for (int rep = 0; rep < 16; ++rep) sink += hrt.count_range(live_probes.front(), live_probes.back());
            t1 = Clock::now();
            for (int rep = 0; rep < 16; ++rep) sink += ost.count_range(live_probes.front(), live_probes.back());
            t2 = Clock::now();
            (void)sink;
            hrt_count.push_back(ns_per_op(t0, t1, 16));
            ost_count.push_back(ns_per_op(t1, t2, 16));
            mem_hrt = hrt.memory_bytes();
            mem_ost = ost.memory_bytes();
            mem_treap = static_cast<size_t>(treap.size()) * 96;
        }
        auto emit = [&](const char* method, const std::vector<double>& samples, size_t mem) {
            if (!first) json << ",\n";
            first = false;
            auto ci = percentile_ci(samples);
            json << "    {\"workload\": "; json_escape(json, w.name);
            json << ", \"method\": \"" << method << "\"";
            json << ", \"rank_ns_median\": " << median(samples);
            json << ", \"rank_ns_p025\": " << ci.first;
            json << ", \"rank_ns_p975\": " << ci.second;
            json << ", \"memory_bytes\": " << mem << "}";
        };
        if (!first) json << ",\n";
        first = false;
        json << "    {\"workload\": "; json_escape(json, w.name);
        json << ", \"method\": \"HRT-LI\"";
        json << ", \"rank_ns_median\": " << median(hrt_rank);
        auto ci = percentile_ci(hrt_rank);
        json << ", \"rank_ns_p025\": " << ci.first << ", \"rank_ns_p975\": " << ci.second;
        json << ", \"count_ns_median\": " << median(hrt_count);
        json << ", \"insert_ns_median\": " << median(hrt_ins);
        json << ", \"delete_ns_median\": " << median(hrt_del);
        json << ", \"memory_bytes\": " << mem_hrt << "}";
        json << ",\n    {\"workload\": "; json_escape(json, w.name);
        json << ", \"method\": \"Counted radix OST\"";
        json << ", \"rank_ns_median\": " << median(ost_rank);
        ci = percentile_ci(ost_rank);
        json << ", \"rank_ns_p025\": " << ci.first << ", \"rank_ns_p975\": " << ci.second;
        json << ", \"count_ns_median\": " << median(ost_count);
        json << ", \"insert_ns_median\": " << median(ost_ins);
        json << ", \"delete_ns_median\": " << median(ost_del);
        json << ", \"memory_bytes\": " << mem_ost << "}";
        json << ",\n    {\"workload\": "; json_escape(json, w.name);
        json << ", \"method\": \"Order-statistic treap\"";
        json << ", \"rank_ns_median\": " << median(treap_rank);
        ci = percentile_ci(treap_rank);
        json << ", \"rank_ns_p025\": " << ci.first << ", \"rank_ns_p975\": " << ci.second;
        json << ", \"count_ns_median\": null";
        json << ", \"insert_ns_median\": null";
        json << ", \"delete_ns_median\": null";
        json << ", \"memory_bytes\": " << mem_treap << "}";
    }
    json << "\n  ],\n";

    json << "  \"mixed_workloads\": [\n";
    first = true;
    for (const auto& w : workloads) {
        for (double ratio : write_ratios) {
            std::vector<double> hrt_ns, ost_ns;
            const size_t ops = 20000;
            int lookups = 0, inserts = 0, deletes = 0;
            int hrt_ok = 0, ost_ok = 0;
            for (int trial = 0; trial < trials; ++trial) {
                MixStream stream = make_mixed_stream(w.base, w.inserts, ratio, trial, ops);
                lookups = stream.lookups;
                inserts = stream.inserts;
                deletes = stream.deletes;
                hrtli::RankTransportIndex hrt(w.base, 64);
                hrtli::CountedRadixOST ost;
                ost.bulk_load(w.base);
                int trial_hrt_ok = 0, trial_ost_ok = 0;
                auto t0 = Clock::now();
                for (const auto& op : stream.ops) {
                    if (op.type == MixOp::Lookup) {
                        (void)hrt.point_lookup(op.key);
                        ++trial_hrt_ok;
                    } else if (op.type == MixOp::Insert) {
                        trial_hrt_ok += int(hrt.insert(op.key));
                    } else {
                        trial_hrt_ok += int(hrt.remove(op.key));
                    }
                }
                auto t1 = Clock::now();
                auto t2 = Clock::now();
                for (const auto& op : stream.ops) {
                    if (op.type == MixOp::Lookup) {
                        (void)ost.point_lookup(op.key);
                        ++trial_ost_ok;
                    } else if (op.type == MixOp::Insert) {
                        trial_ost_ok += int(ost.insert(op.key));
                    } else {
                        trial_ost_ok += int(ost.remove(op.key));
                    }
                }
                auto t3 = Clock::now();
                hrt_ok = trial_hrt_ok;
                ost_ok = trial_ost_ok;
                hrt_ns.push_back(ns_per_op(t0, t1, ops));
                ost_ns.push_back(ns_per_op(t2, t3, ops));
            }
            if (!first) json << ",\n";
            first = false;
            json << "    {\"workload\": "; json_escape(json, w.name);
            json << ", \"write_ratio\": " << ratio;
            json << ", \"lookups\": " << lookups;
            json << ", \"inserts\": " << inserts;
            json << ", \"deletes\": " << deletes;
            json << ", \"hrtli_successes\": " << hrt_ok;
            json << ", \"ost_successes\": " << ost_ok;
            json << ", \"hrtli_ns_median\": " << median(hrt_ns);
            json << ", \"ost_ns_median\": " << median(ost_ns) << "}";
        }
    }
    json << "\n  ],\n";

    json << "  \"delta_growth_consolidation\": [\n";
    first = true;
    for (const auto& w : workloads) {
        hrtli::RankTransportIndex idx(w.base, 64);
        size_t step = std::max<size_t>(1, w.inserts.size() / 8);
        size_t applied = 0;
        int batch = 0;
        auto emit_point = [&](const char* stage) {
            if (!first) json << ",\n";
            first = false;
            json << "    {\"workload\": "; json_escape(json, w.name);
            json << ", \"stage\": \"" << stage << "\"";
            json << ", \"mutations\": " << idx.mutation_count();
            json << ", \"mutation_ratio\": " << idx.mutation_ratio();
            json << ", \"memory_bytes\": " << idx.memory_bytes();
            json << ", \"live_max_error\": " << max_base_error(idx, w.base);
            json << ", \"consolidations\": " << idx.consolidation_count() << "}";
        };
        emit_point("base");
        while (applied < w.inserts.size() && batch < 8) {
            size_t end = std::min(applied + step, w.inserts.size());
            for (; applied < end; ++applied) idx.insert(w.inserts[applied]);
            ++batch;
            emit_point("growth");
        }
        size_t before = idx.mutation_count();
        auto t0 = Clock::now();
        idx.maybe_consolidate(0.01);
        idx.wait_rebuild();
        auto t1 = Clock::now();
        if (!first) json << ",\n";
        first = false;
        json << "    {\"workload\": "; json_escape(json, w.name);
        json << ", \"stage\": \"after_consolidation\"";
        json << ", \"mutations\": " << idx.mutation_count();
        json << ", \"mutation_ratio\": " << idx.mutation_ratio();
        json << ", \"memory_bytes\": " << idx.memory_bytes();
        json << ", \"live_max_error\": " << max_base_error(idx, w.base);
        json << ", \"consolidations\": " << idx.consolidation_count();
        json << ", \"mutations_absorbed\": " << before;
        json << ", \"consolidate_ms\": " << std::chrono::duration<double, std::milli>(t1 - t0).count() << "}";
    }
    json << "\n  ]\n}\n";

    std::ofstream out("results_q1/revision_gap_experiments.json");
    out << json.str();
    std::cout << json.str();
    return 0;
}
