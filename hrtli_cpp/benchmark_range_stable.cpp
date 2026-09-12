#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "rank_transport.hpp"
#include "kinetic_segment_tree.hpp"

using Clock = std::chrono::steady_clock;

struct TrialSamples {
    size_t target = 0;
    double average_result_size = 0.0;
    size_t count_operations = 0;
    size_t scan_operations = 0;
    std::vector<double> count_us;
    std::vector<double> scan_us;
};

std::vector<std::string> read_keys(const std::string& path) {
    std::ifstream input(path);
    std::vector<std::string> keys;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) keys.push_back(line);
    }
    return keys;
}

std::vector<std::pair<std::string, std::string>> make_ranges(
    const std::vector<std::string>& keys, size_t target, size_t count, uint32_t seed) {
    std::vector<std::pair<std::string, std::string>> ranges;
    if (keys.empty()) return ranges;
    std::mt19937 rng(seed);
    const size_t max_start = keys.size() > target ? keys.size() - target : 0;
    std::uniform_int_distribution<size_t> distribution(0, max_start);
    ranges.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t start = distribution(rng);
        const size_t end = std::min(keys.size() - 1, start + target - 1);
        ranges.emplace_back(keys[start], keys[end]);
    }
    return ranges;
}

template <typename Function>
double measure_us_per_operation(Function function, size_t operations) {
    const auto begin = Clock::now();
    function();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::micro>(end - begin).count() /
           static_cast<double>(operations);
}

void write_array(std::ofstream& output, const std::vector<double>& values) {
    output << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) output << ", ";
        output << std::setprecision(12) << values[i];
    }
    output << ']';
}

int main(int argc, char** argv) {
    const std::string dataset = argc > 1 ? argv[1] : "synthetic";
    const std::string output_path = argc > 2
        ? argv[2]
        : "results_q1/benchmark_audit/range_stable_" + dataset + ".json";
    const size_t trials = std::getenv("HRTLI_RANGE_TRIALS")
        ? std::stoull(std::getenv("HRTLI_RANGE_TRIALS")) : 30;
    const size_t minimum_count_operations = std::getenv("HRTLI_RANGE_MIN_COUNTS")
        ? std::stoull(std::getenv("HRTLI_RANGE_MIN_COUNTS")) : 100000;
    const size_t minimum_scan_returned_keys = std::getenv("HRTLI_RANGE_MIN_SCAN_KEYS")
        ? std::stoull(std::getenv("HRTLI_RANGE_MIN_SCAN_KEYS")) : 100000;
    const size_t range_count = 200;

    auto initial = read_keys("data/" + dataset + "_initial.txt");
    auto inserts = read_keys("data/" + dataset + "_insert.txt");
    if (initial.empty()) {
        std::cerr << "missing data for " << dataset << '\n';
        return 1;
    }

    hrtli::RankTransportIndexV3 index(initial, 64);
    for (const auto& key : inserts) index.insert(key);
    std::mt19937 delete_rng(2026);
    std::shuffle(initial.begin(), initial.end(), delete_rng);
    const size_t deletes = std::min<size_t>(1000, initial.size() / 8);
    for (size_t i = 0; i < deletes; ++i) index.remove(initial[i]);
    const auto snapshot = index.snapshot_keys();

    std::vector<TrialSamples> all_samples;
    for (const size_t target : std::vector<size_t>{1, 10, 100, 1000}) {
        const auto ranges = make_ranges(snapshot, target, range_count,
                                        static_cast<uint32_t>(2026 + target));
        size_t returned = 0;
        for (const auto& range : ranges) {
            const int count = index.count_range(range.first, range.second);
            const auto scanned = index.scan_range(range.first, range.second);
            if (count != static_cast<int>(scanned.size())) {
                std::cerr << "count/scan mismatch\n";
                return 2;
            }
            returned += scanned.size();
        }

        const size_t count_repetitions =
            std::max<size_t>(1, (minimum_count_operations + ranges.size() - 1) / ranges.size());
        const size_t scan_repetitions = std::max<size_t>(
            1, (minimum_scan_returned_keys + std::max<size_t>(1, returned) - 1) /
                   std::max<size_t>(1, returned));

        volatile size_t sink = 0;
        auto run_counts = [&]() {
            for (size_t repeat = 0; repeat < count_repetitions; ++repeat)
                for (const auto& range : ranges)
                    sink += static_cast<size_t>(index.count_range(range.first, range.second));
        };
        auto run_scans = [&]() {
            for (size_t repeat = 0; repeat < scan_repetitions; ++repeat)
                for (const auto& range : ranges)
                    sink += index.scan_range(range.first, range.second).size();
        };
        run_counts();
        run_scans();

        TrialSamples samples;
        samples.target = target;
        samples.average_result_size = static_cast<double>(returned) / ranges.size();
        samples.count_operations = count_repetitions * ranges.size();
        samples.scan_operations = scan_repetitions * ranges.size();
        for (size_t trial = 0; trial < trials; ++trial) {
            if (trial % 2 == 0) {
                samples.count_us.push_back(measure_us_per_operation(run_counts, samples.count_operations));
                samples.scan_us.push_back(measure_us_per_operation(run_scans, samples.scan_operations));
            } else {
                samples.scan_us.push_back(measure_us_per_operation(run_scans, samples.scan_operations));
                samples.count_us.push_back(measure_us_per_operation(run_counts, samples.count_operations));
            }
        }
        all_samples.push_back(std::move(samples));
        std::cout << dataset << " k=" << target << " complete\n";
        (void)sink;
    }

    std::ofstream output(output_path);
    output << "{\n  \"dataset\": \"" << dataset << "\",\n"
           << "  \"method\": \"native_batched_steady_clock\",\n"
           << "  \"trials\": " << trials << ",\n"
           << "  \"base_keys\": " << initial.size() << ",\n"
           << "  \"insert_keys\": " << inserts.size() << ",\n"
           << "  \"deleted_keys\": " << deletes << ",\n"
           << "  \"live_keys\": " << snapshot.size() << ",\n"
           << "  \"selectivities\": {\n";
    for (size_t i = 0; i < all_samples.size(); ++i) {
        const auto& samples = all_samples[i];
        output << "    \"" << samples.target << "\": {\n"
               << "      \"target_result_size\": " << samples.target << ",\n"
               << "      \"average_result_size\": " << samples.average_result_size << ",\n"
               << "      \"count_operations_per_trial\": " << samples.count_operations << ",\n"
               << "      \"scan_operations_per_trial\": " << samples.scan_operations << ",\n"
               << "      \"count_us_samples\": ";
        write_array(output, samples.count_us);
        output << ",\n      \"scan_us_samples\": ";
        write_array(output, samples.scan_us);
        output << "\n    }" << (i + 1 == all_samples.size() ? "\n" : ",\n");
    }
    output << "  }\n}\n";
    return 0;
}
