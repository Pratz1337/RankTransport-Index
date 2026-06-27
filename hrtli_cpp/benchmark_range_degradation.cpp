#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::high_resolution_clock;

struct RangeQuery {
    std::string lo_key;
    std::string hi_key;
    double lo_surrogate;
    double hi_surrogate;
};

struct BenchmarkConfig {
    std::vector<std::string> datasets;
    std::vector<double> deletion_ratios{0.0, 0.2, 0.5, 0.8, 0.95};
    std::vector<double> tau_values;
    size_t target_live_scan_size = 100;
    size_t num_queries = 200;
    int trials = 1;
};

struct ScanSummary {
    double mean_total_scan_time_ms = 0.0;
    double mean_avg_scan_time_us = 0.0;
    double ci95_avg_scan_time_us = 0.0;
    double mean_total_returned = 0.0;
    int trials = 0;
};

struct DegradationRow {
    std::string dataset;
    double deleted_ratio;
    std::string index_name;
    double total_scan_time_ms;
    double avg_scan_time_us;
    double ci95_avg_scan_time_us;
    double total_returned;
    double maintenance_time_ms = 0.0;
    int trials = 1;
};

#if defined(HRTLI_WITH_HOT)
struct StringPayload {
    const char* key;
    uint64_t value;
};

template <typename ValueType>
struct StringPayloadExtractor {
    using KeyType = const char*;
    KeyType operator()(ValueType const& payload) const { return payload->key; }
    KeyType operator()(KeyType key) const { return key; }
};
#endif

std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream file(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            keys.push_back(line);
        }
    }
    return keys;
}

std::vector<std::string> sorted_unique(std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

double ms_since(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return std::accumulate(values.begin(), values.end(), 0.0) /
           static_cast<double>(values.size());
}

double ci95(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    double mu = mean(values);
    double sum_sq = 0.0;
    for (double v : values) {
        double diff = v - mu;
        sum_sq += diff * diff;
    }
    double sample_var = sum_sq / static_cast<double>(values.size() - 1);
    return 1.96 * std::sqrt(sample_var / static_cast<double>(values.size()));
}

std::vector<double> parse_double_list(const std::string& raw) {
    std::vector<double> values;
    std::stringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (!token.empty()) {
            values.push_back(std::stod(token));
        }
    }
    return values;
}

std::unordered_map<std::string, double> build_surrogate_keys(
    const std::vector<std::string>& all_keys) {
    std::unordered_map<std::string, double> out;
    out.reserve(all_keys.size());
    for (size_t i = 0; i < all_keys.size(); ++i) {
        out.emplace(all_keys[i], static_cast<double>(i + 1));
    }
    return out;
}

template <typename Fn>
ScanSummary measure_scan(int trials, size_t num_queries, Fn&& fn) {
    std::vector<double> avg_us_values;
    std::vector<double> total_ms_values;
    std::vector<double> returned_values;
    avg_us_values.reserve(trials);
    total_ms_values.reserve(trials);
    returned_values.reserve(trials);

    for (int t = 0; t < trials; ++t) {
        auto start = Clock::now();
        size_t returned = fn();
        auto end = Clock::now();
        double total_ms = ms_since(start, end);
        total_ms_values.push_back(total_ms);
        avg_us_values.push_back((total_ms * 1000.0) / static_cast<double>(num_queries));
        returned_values.push_back(static_cast<double>(returned));
    }

    return {
        mean(total_ms_values),
        mean(avg_us_values),
        ci95(avg_us_values),
        mean(returned_values),
        trials
    };
}

void append_result(std::vector<DegradationRow>& all_results,
                   const std::string& dataset_name,
                   double deleted_ratio,
                   const std::string& index_name,
                   const ScanSummary& summary,
                   double maintenance_time_ms) {
    all_results.push_back({
        dataset_name,
        deleted_ratio,
        index_name,
        summary.mean_total_scan_time_ms,
        summary.mean_avg_scan_time_us,
        summary.ci95_avg_scan_time_us,
        summary.mean_total_returned,
        maintenance_time_ms,
        summary.trials
    });
}

struct TauState {
    double tau;
    std::unique_ptr<hrtli::RankTransportIndex> index;
    double cumulative_maintenance_time_ms = 0.0;
};

std::vector<RangeQuery> build_fixed_output_queries(
    const std::vector<std::string>& live_keys,
    const std::unordered_map<std::string, double>& surrogate,
    size_t target_live_scan_size,
    size_t num_queries,
    std::mt19937& rng) {
    if (live_keys.size() <= target_live_scan_size) {
        return {};
    }
    std::vector<RangeQuery> queries;
    queries.reserve(num_queries);
    std::uniform_int_distribution<size_t> dist(
        0, live_keys.size() - target_live_scan_size - 1);

    for (size_t i = 0; i < num_queries; ++i) {
        size_t idx = dist(rng);
        const std::string& lo = live_keys[idx];
        const std::string& hi = live_keys[idx + target_live_scan_size - 1];
        queries.push_back({lo, hi, surrogate.at(lo), surrogate.at(hi)});
    }
    return queries;
}

void run_degradation_for_dataset(const std::string& dataset_name,
                                 const BenchmarkConfig& config,
                                 std::vector<DegradationRow>& all_results) {
    auto initial_keys = read_keys("data/" + dataset_name + "_initial.txt");
    if (initial_keys.empty()) {
        std::cerr << "Warning: Missing or empty dataset files for "
                  << dataset_name << ", skipping.\n";
        return;
    }

    initial_keys = sorted_unique(std::move(initial_keys));
    const size_t N = initial_keys.size();
    std::cout << "Loaded dataset: " << dataset_name << " with "
              << N << " unique sorted keys.\n";

    if (N <= config.target_live_scan_size + 1) {
        std::cerr << "Dataset too small for fixed-output range benchmark.\n";
        return;
    }

    auto surrogate = build_surrogate_keys(initial_keys);

    std::vector<std::pair<double, uint64_t>> bulk;
    bulk.reserve(N);
    for (size_t i = 0; i < N; ++i) {
        bulk.emplace_back(surrogate.at(initial_keys[i]), i + 1);
    }
    std::sort(bulk.begin(), bulk.end());

    hrtli::external::DynamicPgmIndex<double, uint64_t> pgm_nocompact(
        bulk.begin(), bulk.end());
    std::set<std::string> ordered_tree(initial_keys.begin(), initial_keys.end());
    hrtli::RankTransportIndex hrtli_nocons(initial_keys, 64);
    hrtli::RankTransportIndexV3 hrtli_v3(initial_keys, 64);
    hrtli::RankTransportIndex hrtli_step_cons(initial_keys, 64);

    std::vector<TauState> tau_states;
    tau_states.reserve(config.tau_values.size());
    for (double tau : config.tau_values) {
        tau_states.push_back({
            tau,
            std::make_unique<hrtli::RankTransportIndex>(initial_keys, 64),
            0.0
        });
    }

#if defined(HRTLI_WITH_HOT)
    hot::singlethreaded::HOTSingleThreaded<StringPayload*, StringPayloadExtractor> hot_index;
    std::vector<std::unique_ptr<StringPayload>> hot_payloads;
    hot_payloads.reserve(initial_keys.size());
    for (size_t i = 0; i < initial_keys.size(); ++i) {
        hot_payloads.push_back(std::make_unique<StringPayload>(
            StringPayload{initial_keys[i].c_str(), i + 1}));
        hot_index.insert(hot_payloads.back().get());
    }
#endif

    std::vector<std::string> keys_to_delete = initial_keys;
    std::mt19937 rng(2026);
    std::shuffle(keys_to_delete.begin(), keys_to_delete.end(), rng);
    size_t last_deleted_count = 0;

    for (double d : config.deletion_ratios) {
        size_t target_deleted_count = static_cast<size_t>(std::round(N * d));
        target_deleted_count = std::min(target_deleted_count, N);

        for (size_t i = last_deleted_count; i < target_deleted_count; ++i) {
            const auto& key = keys_to_delete[i];
            pgm_nocompact.erase(surrogate.at(key));
            ordered_tree.erase(key);
            hrtli_nocons.remove(key);
            hrtli_v3.remove(key);
            hrtli_step_cons.remove(key);

            for (auto& state : tau_states) {
                state.index->remove(key);
                auto start = Clock::now();
                bool started = state.index->maybe_consolidate(state.tau);
                if (started) {
                    state.index->wait_rebuild();
                    auto end = Clock::now();
                    state.cumulative_maintenance_time_ms += ms_since(start, end);
                }
            }

#if defined(HRTLI_WITH_HOT)
            hot_index.remove(key.c_str());
#endif
        }

        double hrtli_step_maintenance_ms = 0.0;
        if (d > 0.0) {
            auto cons_start = Clock::now();
            hrtli_step_cons.consolidate();
            hrtli_step_cons.wait_rebuild();
            auto cons_end = Clock::now();
            hrtli_step_maintenance_ms = ms_since(cons_start, cons_end);
        }

        last_deleted_count = target_deleted_count;

        std::unordered_set<std::string> deleted_keys;
        deleted_keys.reserve(target_deleted_count * 2 + 1);
        for (size_t i = 0; i < target_deleted_count; ++i) {
            deleted_keys.insert(keys_to_delete[i]);
        }

        std::vector<std::string> live_keys;
        live_keys.reserve(N - target_deleted_count);
        for (const auto& key : initial_keys) {
            if (deleted_keys.find(key) == deleted_keys.end()) {
                live_keys.push_back(key);
            }
        }

        if (live_keys.size() <= config.target_live_scan_size) {
            std::cerr << "Not enough live keys for fixed-output range queries at deletion ratio "
                      << d << "\n";
            continue;
        }

        auto queries = build_fixed_output_queries(
            live_keys,
            surrogate,
            config.target_live_scan_size,
            config.num_queries,
            rng);

        std::cout << "  Deletion ratio: " << (d * 100.0)
                  << "% (deleted: " << target_deleted_count << "/" << N
                  << ", trials: " << config.trials << ")\n";

        auto pgm_no_compact = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                returned += pgm_nocompact.range(q.lo_surrogate, q.hi_surrogate).size();
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "Dynamic PGM (no compaction)",
                      pgm_no_compact, 0.0);

        std::vector<std::pair<double, uint64_t>> live_bulk;
        live_bulk.reserve(live_keys.size());
        for (size_t i = 0; i < live_keys.size(); ++i) {
            live_bulk.emplace_back(surrogate.at(live_keys[i]), i + 1);
        }
        std::sort(live_bulk.begin(), live_bulk.end());
        auto pgm_build_start = Clock::now();
        auto pgm_compacted = std::make_unique<
            hrtli::external::DynamicPgmIndex<double, uint64_t>>(
                live_bulk.begin(), live_bulk.end());
        auto pgm_build_end = Clock::now();
        double pgm_compaction_ms = ms_since(pgm_build_start, pgm_build_end);

        auto pgm_compact = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                returned += pgm_compacted->range(q.lo_surrogate, q.hi_surrogate).size();
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "Dynamic PGM (with compaction)",
                      pgm_compact, pgm_compaction_ms);

        auto set_scan = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                auto it = ordered_tree.lower_bound(q.lo_key);
                while (it != ordered_tree.end() && *it <= q.hi_key) {
                    ++returned;
                    ++it;
                }
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "std::set ordered tree",
                      set_scan, 0.0);

#if defined(HRTLI_WITH_HOT)
        auto hot_scan = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                auto it = hot_index.lower_bound(q.lo_key.c_str());
                auto end = hot_index.end();
                while (it != end) {
                    StringPayload* payload = *it;
                    if (std::strcmp(payload->key, q.hi_key.c_str()) > 0) {
                        break;
                    }
                    ++returned;
                    ++it;
                }
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "HOT string",
                      hot_scan, 0.0);
#endif

        auto hrtli_no_cons = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                auto res = hrtli_nocons.scan_range(q.lo_key, q.hi_key);
                returned += res.size();
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "HRT-LI (no consolidation)",
                      hrtli_no_cons, 0.0);

        auto hrtli_step = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                auto res = hrtli_step_cons.scan_range(q.lo_key, q.hi_key);
                returned += res.size();
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "HRT-LI (step consolidation)",
                      hrtli_step, hrtli_step_maintenance_ms);

        auto hrtli_v3_scan = measure_scan(config.trials, queries.size(), [&]() {
            size_t returned = 0;
            for (const auto& q : queries) {
                auto res = hrtli_v3.scan_range(q.lo_key, q.hi_key);
                returned += res.size();
            }
            return returned;
        });
        append_result(all_results, dataset_name, d, "HRT-LI v3",
                      hrtli_v3_scan, 0.0);

        for (const auto& state : tau_states) {
            auto tau_scan = measure_scan(config.trials, queries.size(), [&]() {
                size_t returned = 0;
                for (const auto& q : queries) {
                    auto res = state.index->scan_range(q.lo_key, q.hi_key);
                    returned += res.size();
                }
                return returned;
            });
            std::ostringstream name;
            name << "HRT-LI (tau=" << state.tau << ")";
            append_result(all_results, dataset_name, d, name.str(), tau_scan,
                          state.cumulative_maintenance_time_ms);
        }
    }
}

BenchmarkConfig parse_args(int argc, char** argv) {
    BenchmarkConfig config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(flag + " requires a value");
            }
            return argv[++i];
        };

        if (arg == "--trials") {
            config.trials = std::max(1, std::stoi(require_value(arg)));
        } else if (arg.rfind("--trials=", 0) == 0) {
            config.trials = std::max(1, std::stoi(arg.substr(9)));
        } else if (arg == "--queries") {
            config.num_queries = static_cast<size_t>(std::stoull(require_value(arg)));
        } else if (arg.rfind("--queries=", 0) == 0) {
            config.num_queries = static_cast<size_t>(std::stoull(arg.substr(10)));
        } else if (arg == "--scan-size") {
            config.target_live_scan_size = static_cast<size_t>(std::stoull(require_value(arg)));
        } else if (arg.rfind("--scan-size=", 0) == 0) {
            config.target_live_scan_size = static_cast<size_t>(std::stoull(arg.substr(12)));
        } else if (arg == "--ratios") {
            config.deletion_ratios = parse_double_list(require_value(arg));
        } else if (arg.rfind("--ratios=", 0) == 0) {
            config.deletion_ratios = parse_double_list(arg.substr(9));
        } else if (arg == "--tau") {
            config.tau_values = parse_double_list(require_value(arg));
        } else if (arg.rfind("--tau=", 0) == 0) {
            config.tau_values = parse_double_list(arg.substr(6));
        } else if (arg == "--tau-sweep") {
            config.tau_values = {0.05, 0.10, 0.20, 0.30};
        } else {
            config.datasets.push_back(arg);
        }
    }
    if (config.datasets.empty()) {
        config.datasets = {"synthetic", "dns"};
    }
    if (config.num_queries == 0 || config.target_live_scan_size == 0) {
        throw std::invalid_argument("--queries and --scan-size must be positive");
    }
    std::sort(config.deletion_ratios.begin(), config.deletion_ratios.end());
    return config;
}

int main(int argc, char** argv) {
    BenchmarkConfig config;
    try {
        config = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 2;
    }

    std::vector<DegradationRow> all_results;
    for (const auto& dataset : config.datasets) {
        run_degradation_for_dataset(dataset, config, all_results);
    }

    std::ofstream csv_out("results_q1/range_degradation_results.csv");
    if (csv_out) {
        csv_out << "dataset,deleted_ratio,index_name,total_scan_time_ms,"
                   "avg_scan_time_us,ci95_avg_scan_time_us,total_returned,"
                   "maintenance_time_ms,trials\n";
        for (const auto& row : all_results) {
            csv_out << row.dataset << "," << row.deleted_ratio << ","
                    << row.index_name << "," << row.total_scan_time_ms << ","
                    << row.avg_scan_time_us << "," << row.ci95_avg_scan_time_us
                    << "," << row.total_returned << "," << row.maintenance_time_ms
                    << "," << row.trials << "\n";
        }
        std::cout << "Wrote range degradation results to "
                  << "results_q1/range_degradation_results.csv\n";
    }

    std::ofstream json_out("results_q1/range_degradation_results.json");
    if (json_out) {
        json_out << "[\n";
        for (size_t i = 0; i < all_results.size(); ++i) {
            const auto& row = all_results[i];
            json_out << "  {\n"
                     << "    \"dataset\": \"" << row.dataset << "\",\n"
                     << "    \"deleted_ratio\": " << row.deleted_ratio << ",\n"
                     << "    \"index_name\": \"" << row.index_name << "\",\n"
                     << "    \"total_scan_time_ms\": " << row.total_scan_time_ms << ",\n"
                     << "    \"avg_scan_time_us\": " << row.avg_scan_time_us << ",\n"
                     << "    \"ci95_avg_scan_time_us\": " << row.ci95_avg_scan_time_us << ",\n"
                     << "    \"total_returned\": " << row.total_returned << ",\n"
                     << "    \"maintenance_time_ms\": " << row.maintenance_time_ms << ",\n"
                     << "    \"trials\": " << row.trials << "\n"
                     << "  }" << (i + 1 == all_results.size() ? "" : ",") << "\n";
        }
        json_out << "]\n";
        std::cout << "Wrote range degradation results to "
                  << "results_q1/range_degradation_results.json\n";
    }

    return 0;
}
