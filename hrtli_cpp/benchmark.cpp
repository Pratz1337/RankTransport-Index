#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "external_baseline_bindings.hpp"
#include "rank_transport.hpp"

using Clock = std::chrono::high_resolution_clock;

struct BenchmarkResult {
    std::string index_name;
    double load_time_ms = 0.0;
    double insert_time_ms = 0.0;
    double lookup_throughput_mops = 0.0;
    double avg_lookup_latency_ns = 0.0;
    size_t memory_bytes = 0;
    size_t timed_lookup_operations = 0;
    size_t trials = 1;
    double load_time_ms_ci95 = 0.0;
    double insert_time_ms_ci95 = 0.0;
    double lookup_throughput_mops_ci95 = 0.0;
    double avg_lookup_latency_ns_ci95 = 0.0;
    std::vector<double> load_time_ms_samples;
    std::vector<double> insert_time_ms_samples;
    std::vector<double> lookup_throughput_mops_samples;
    std::vector<double> avg_lookup_latency_ns_samples;
};

size_t min_timed_lookup_operations = 1000000;

size_t timed_lookup_repetitions(size_t query_count) {
    if (query_count == 0) return 0;
    return std::max<size_t>(1, (min_timed_lookup_operations + query_count - 1) / query_count);
}

struct MeanCi95 {
    double mean = 0.0;
    double ci95 = 0.0;
};

double t_critical_95(size_t degrees_of_freedom) {
    static constexpr double critical[] = {
        0.0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365,
        2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131,
        2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069,
        2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042
    };
    if (degrees_of_freedom < sizeof(critical) / sizeof(critical[0])) {
        return critical[degrees_of_freedom];
    }
    return 1.96;
}

MeanCi95 summarize(const std::vector<double>& values) {
    MeanCi95 result;
    if (values.empty()) return result;
    for (double value : values) result.mean += value;
    result.mean /= static_cast<double>(values.size());
    if (values.size() == 1) return result;

    double sum_squared_deviation = 0.0;
    for (double value : values) {
        const double deviation = value - result.mean;
        sum_squared_deviation += deviation * deviation;
    }
    const double sample_sd = std::sqrt(
        sum_squared_deviation / static_cast<double>(values.size() - 1));
    result.ci95 = t_critical_95(values.size() - 1) * sample_sd /
                  std::sqrt(static_cast<double>(values.size()));
    return result;
}

BenchmarkResult aggregate_trials(const std::vector<BenchmarkResult>& samples) {
    BenchmarkResult result;
    result.index_name = samples.front().index_name;
    result.trials = samples.size();
    result.timed_lookup_operations = samples.front().timed_lookup_operations;
    for (const auto& sample : samples) {
        result.load_time_ms_samples.push_back(sample.load_time_ms);
        result.insert_time_ms_samples.push_back(sample.insert_time_ms);
        result.lookup_throughput_mops_samples.push_back(sample.lookup_throughput_mops);
        result.avg_lookup_latency_ns_samples.push_back(sample.avg_lookup_latency_ns);
        result.memory_bytes = std::max(result.memory_bytes, sample.memory_bytes);
    }
    const auto load = summarize(result.load_time_ms_samples);
    const auto insert = summarize(result.insert_time_ms_samples);
    const auto throughput = summarize(result.lookup_throughput_mops_samples);
    const auto latency = summarize(result.avg_lookup_latency_ns_samples);
    result.load_time_ms = load.mean;
    result.load_time_ms_ci95 = load.ci95;
    result.insert_time_ms = insert.mean;
    result.insert_time_ms_ci95 = insert.ci95;
    result.lookup_throughput_mops = throughput.mean;
    result.lookup_throughput_mops_ci95 = throughput.ci95;
    result.avg_lookup_latency_ns = latency.mean;
    result.avg_lookup_latency_ns_ci95 = latency.ci95;
    return result;
}

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

double elapsed_ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void fill_lookup_stats(BenchmarkResult& result, size_t n_lookup, double lookup_ms) {
    result.lookup_throughput_mops = (static_cast<double>(n_lookup) / (lookup_ms / 1000.0)) / 1e6;
    result.avg_lookup_latency_ns = (lookup_ms * 1e6) / static_cast<double>(n_lookup);
}

BenchmarkResult bench_hrtli(const std::vector<std::string>& initial,
                            const std::vector<std::string>& inserts,
                            const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "HRT-LI native rank transport";

    auto start = Clock::now();
    hrtli::RankTransportIndex index(initial, 64);
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        index.insert(key);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    for (const auto& key : lookups) {
        sink += (index.point_lookup(key) ? 1 : 0);
    }
    const size_t repetitions = timed_lookup_repetitions(lookups.size());
    start = Clock::now();
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& key : lookups) {
            sink += (index.point_lookup(key) ? 1 : 0);
        }
    }
    end = Clock::now();
    result.timed_lookup_operations = lookups.size() * repetitions;
    fill_lookup_stats(result, result.timed_lookup_operations, elapsed_ms(start, end));
    result.memory_bytes = index.memory_bytes();
    (void)sink;
    return result;
}

BenchmarkResult bench_sorted_vector(const std::vector<std::string>& initial,
                                    const std::vector<std::string>& inserts,
                                    const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "sorted vector lower_bound";

    auto start = Clock::now();
    std::vector<std::string> keys = sorted_unique(initial);
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        if (it == keys.end() || *it != key) {
            keys.insert(it, key);
        }
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    for (const auto& key : lookups) {
        auto it = std::lower_bound(keys.begin(), keys.end(), key);
        sink += ((it != keys.end() && *it == key) ? 1 : 0);
    }
    const size_t repetitions = timed_lookup_repetitions(lookups.size());
    start = Clock::now();
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& key : lookups) {
            auto it = std::lower_bound(keys.begin(), keys.end(), key);
            sink += ((it != keys.end() && *it == key) ? 1 : 0);
        }
    }
    end = Clock::now();
    result.timed_lookup_operations = lookups.size() * repetitions;
    fill_lookup_stats(result, result.timed_lookup_operations, elapsed_ms(start, end));
    result.memory_bytes = keys.size() * sizeof(std::string);
    for (const auto& key : keys) {
        result.memory_bytes += key.capacity();
    }
    (void)sink;
    return result;
}

BenchmarkResult bench_std_set(const std::vector<std::string>& initial,
                              const std::vector<std::string>& inserts,
                              const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "std::set";

    auto start = Clock::now();
    std::set<std::string> keys(initial.begin(), initial.end());
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (const auto& key : inserts) {
        keys.insert(key);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile int64_t sink = 0;
    for (const auto& key : lookups) {
        sink += (keys.find(key) != keys.end() ? 1 : 0);
    }
    const size_t repetitions = timed_lookup_repetitions(lookups.size());
    start = Clock::now();
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& key : lookups) {
            sink += (keys.find(key) != keys.end() ? 1 : 0);
        }
    }
    end = Clock::now();
    result.timed_lookup_operations = lookups.size() * repetitions;
    fill_lookup_stats(result, result.timed_lookup_operations, elapsed_ms(start, end));
    result.memory_bytes = keys.size() * 96;
    (void)sink;
    return result;
}

BenchmarkResult bench_unordered_map(const std::vector<std::string>& initial,
                                    const std::vector<std::string>& inserts,
                                    const std::vector<std::string>& lookups) {
    BenchmarkResult result;
    result.index_name = "std::unordered_map membership reference";

    auto start = Clock::now();
    std::unordered_map<std::string, uint64_t> keys;
    keys.reserve(initial.size() + inserts.size());
    for (size_t i = 0; i < initial.size(); ++i) {
        keys.emplace(initial[i], i);
    }
    auto end = Clock::now();
    result.load_time_ms = elapsed_ms(start, end);

    start = Clock::now();
    for (size_t i = 0; i < inserts.size(); ++i) {
        keys.emplace(inserts[i], initial.size() + i);
    }
    end = Clock::now();
    result.insert_time_ms = elapsed_ms(start, end);

    volatile uint64_t sink = 0;
    for (const auto& key : lookups) {
        auto it = keys.find(key);
        if (it != keys.end()) sink += it->second;
    }
    const size_t repetitions = timed_lookup_repetitions(lookups.size());
    start = Clock::now();
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const auto& key : lookups) {
            auto it = keys.find(key);
            if (it != keys.end()) {
                sink += it->second;
            }
        }
    }
    end = Clock::now();
    result.timed_lookup_operations = lookups.size() * repetitions;
    fill_lookup_stats(result, result.timed_lookup_operations, elapsed_ms(start, end));
    result.memory_bytes = keys.size() * 112;
    (void)sink;
    return result;
}

std::vector<BenchmarkResult> run_dataset(const std::string& dataset, size_t trials,
                                         bool ordered_only) {
    auto initial = read_keys("data/" + dataset + "_initial.txt");
    auto inserts = read_keys("data/" + dataset + "_insert.txt");
    if (initial.empty() || inserts.empty()) {
        return {};
    }

    std::vector<std::string> lookups = initial;
    lookups.insert(lookups.end(), inserts.begin(), inserts.end());
    lookups = sorted_unique(std::move(lookups));
    std::mt19937 rng(2026);
    std::shuffle(lookups.begin(), lookups.end(), rng);

    using Bench = std::function<BenchmarkResult()>;
    std::vector<Bench> benches;
    benches.push_back([&] { return bench_hrtli(initial, inserts, lookups); });
    if (initial.size() + inserts.size() < 1000000) {
        benches.push_back([&] { return bench_sorted_vector(initial, inserts, lookups); });
        benches.push_back([&] { return bench_std_set(initial, inserts, lookups); });
    }
    if (!ordered_only) {
        benches.push_back([&] { return bench_unordered_map(initial, inserts, lookups); });
    }

    std::vector<std::vector<BenchmarkResult>> samples_by_index(benches.size());
    for (size_t trial = 0; trial < trials; ++trial) {
        for (size_t step = 0; step < benches.size(); ++step) {
            const size_t index = (trial + step) % benches.size();
            samples_by_index[index].push_back(benches[index]());
        }
    }
    if (initial.size() + inserts.size() >= 1000000) {
        std::cout << "  (Skipping sorted vector and std::set on large dataset to avoid hang/OOM)\n";
    }

    std::vector<BenchmarkResult> results;
    for (const auto& samples : samples_by_index) {
        results.push_back(aggregate_trials(samples));
    }

    std::cout << "\nDataset: " << dataset << " (" << initial.size()
              << " initial, " << inserts.size() << " inserts)\n";
    std::cout << std::left << std::setw(34) << "Index"
              << std::right << std::setw(12) << "Load ms"
              << std::setw(12) << "Insert ms"
              << std::setw(14) << "Lookup Mops"
              << std::setw(14) << "Latency ns"
              << std::setw(12) << "Memory KB"
              << std::setw(8) << "Trials" << "\n";
    std::cout << std::string(106, '-') << "\n";
    for (const auto& row : results) {
        std::cout << std::left << std::setw(34) << row.index_name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(3) << row.load_time_ms
                  << std::setw(12) << row.insert_time_ms
                  << std::setw(14) << row.lookup_throughput_mops
                  << std::setw(14) << row.avg_lookup_latency_ns
                  << std::setw(12) << (row.memory_bytes / 1024.0)
                  << std::setw(8) << row.trials << "\n";
    }
    return results;
}

void write_json(const std::string& path,
                const std::vector<std::pair<std::string, std::vector<BenchmarkResult>>>& all_results) {
    std::ofstream file(path);
    file << "{\n";
    for (size_t i = 0; i < all_results.size(); ++i) {
        const auto& dataset = all_results[i].first;
        const auto& rows = all_results[i].second;
        file << "  \"" << dataset << "\": [\n";
        for (size_t j = 0; j < rows.size(); ++j) {
            const auto& row = rows[j];
            file << "    {"
                 << "\"index_name\": \"" << row.index_name << "\", "
                 << "\"load_time_ms\": " << row.load_time_ms << ", "
                 << "\"load_time_ms_ci95\": " << row.load_time_ms_ci95 << ", "
                 << "\"insert_time_ms\": " << row.insert_time_ms << ", "
                 << "\"insert_time_ms_ci95\": " << row.insert_time_ms_ci95 << ", "
                 << "\"lookup_throughput_mops\": " << row.lookup_throughput_mops << ", "
                 << "\"lookup_throughput_mops_ci95\": " << row.lookup_throughput_mops_ci95 << ", "
                 << "\"avg_lookup_latency_ns\": " << row.avg_lookup_latency_ns << ", "
                 << "\"avg_lookup_latency_ns_ci95\": " << row.avg_lookup_latency_ns_ci95 << ", "
                 << "\"memory_bytes\": " << row.memory_bytes << ", "
                 << "\"timed_lookup_operations\": " << row.timed_lookup_operations << ", "
                 << "\"trials\": " << row.trials << ", "
                 << "\"samples\": {";
            const auto write_samples = [&file](const char* name, const std::vector<double>& values) {
                file << "\"" << name << "\": [";
                for (size_t k = 0; k < values.size(); ++k) {
                    if (k > 0) file << ", ";
                    file << values[k];
                }
                file << "]";
            };
            write_samples("load_time_ms", row.load_time_ms_samples);
            file << ", ";
            write_samples("insert_time_ms", row.insert_time_ms_samples);
            file << ", ";
            write_samples("lookup_throughput_mops", row.lookup_throughput_mops_samples);
            file << ", ";
            write_samples("avg_lookup_latency_ns", row.avg_lookup_latency_ns_samples);
            file << "}}";
            file << (j + 1 == rows.size() ? "\n" : ",\n");
        }
        file << "  ]" << (i + 1 == all_results.size() ? "\n" : ",\n");
    }
    file << "}\n";
}

int main(int argc, char** argv) {
    size_t trials = 1;
    bool small_only = false;
    bool ordered_only = false;
    std::vector<std::string> requested_datasets;
    std::string output_path = "results_q1/cpp_benchmark_results.json";
    if (const char* env_trials = std::getenv("HRTLI_NATIVE_TRIALS")) {
        trials = static_cast<size_t>(std::stoul(env_trials));
    }
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--trials" && i + 1 < argc) {
            trials = static_cast<size_t>(std::stoul(argv[++i]));
        } else if (std::string(argv[i]) == "--small") {
            small_only = true;
        } else if (std::string(argv[i]) == "--ordered-only") {
            ordered_only = true;
        } else if (std::string(argv[i]) == "--dataset" && i + 1 < argc) {
            requested_datasets.push_back(argv[++i]);
        } else if (std::string(argv[i]) == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (std::string(argv[i]) == "--min-lookup-operations" && i + 1 < argc) {
            min_timed_lookup_operations = static_cast<size_t>(std::stoull(argv[++i]));
        } else {
            std::cerr << "Usage: " << argv[0]
                      << " [--trials N] [--small] [--output PATH]"
                      << " [--min-lookup-operations N] [--ordered-only]"
                      << " [--dataset NAME ...]\n";
            return 2;
        }
    }
    if (trials == 0) {
        std::cerr << "Trial count must be positive.\n";
        return 2;
    }

    const std::vector<std::string> datasets = !requested_datasets.empty()
        ? requested_datasets
        : (small_only
            ? std::vector<std::string>{"synthetic", "filesystem", "url", "dns", "json", "package"}
            : std::vector<std::string>{"synthetic", "filesystem", "url", "dns", "json", "package", "packages", "wiki_ts", "osm_cellids"});
    std::vector<std::pair<std::string, std::vector<BenchmarkResult>>> all_results;
    for (const auto& dataset : datasets) {
        auto rows = run_dataset(dataset, trials, ordered_only);
        if (!rows.empty()) {
            all_results.emplace_back(dataset, std::move(rows));
        }
    }
    write_json(output_path, all_results);
    std::cout << "\nSaved native benchmark results to " << output_path << "\n";
    std::cout << "External baseline binding macros available:";
    for (const auto& spec : hrtli::external::binding_specs()) {
        std::cout << " " << spec.macro;
    }
    std::cout << "\n";
    return 0;
}
