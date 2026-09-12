#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "packed_rank_transport.hpp"

using Clock = std::chrono::steady_clock;

static double millis(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

static std::vector<std::string> read_keys(const std::string& path,
                                          size_t expected) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open insert file");
    std::vector<std::string> keys;
    keys.reserve(expected);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) keys.push_back(line);
    }
    if (keys.size() != expected || !std::is_sorted(keys.begin(), keys.end()) ||
        std::adjacent_find(keys.begin(), keys.end()) != keys.end()) {
        throw std::runtime_error("insert file count/order mismatch");
    }
    return keys;
}

static uint64_t validate_insert_ranks(const hrtli::PackedRankTransportIndex& index,
                                      const std::vector<std::string>& inserts) {
    uint64_t checksum = 0;
    for (size_t i = 0; i < inserts.size(); ++i) {
        const int expected = static_cast<int>(
            index.base_store().lower_bound(inserts[i]) + i);
        const int actual = index.binary_exact_rank(inserts[i]);
        if (actual != expected) throw std::runtime_error("insert-rank mismatch");
        checksum += static_cast<uint64_t>(actual);
    }
    return checksum;
}

static int run_churn(const std::string& base_path,
                     const std::vector<std::string>& inserts,
                     size_t base_count) {
    const auto build_begin = Clock::now();
    hrtli::PackedRankTransportIndex index(base_path, base_count, 64, true);
    const auto build_end = Clock::now();

    const auto insert_begin = Clock::now();
    size_t inserted = 0;
    for (const auto& key : inserts) inserted += index.insert(key) ? 1U : 0U;
    const auto insert_end = Clock::now();
    const uint64_t before_checksum = validate_insert_ranks(index, inserts);

    const auto delete_begin = Clock::now();
    size_t deleted = 0;
    for (const auto& key : inserts) deleted += index.remove(key) ? 1U : 0U;
    const auto delete_end = Clock::now();
    if (index.mutation_count() != 0) {
        throw std::runtime_error("insert-delete annihilation left ledger state");
    }
    for (const auto& key : inserts) {
        if (index.point_lookup(key)) {
            throw std::runtime_error("deleted held-out key remains live");
        }
    }

    const auto reinsert_begin = Clock::now();
    size_t reinserted = 0;
    for (const auto& key : inserts) reinserted += index.insert(key) ? 1U : 0U;
    const auto reinsert_end = Clock::now();
    const uint64_t after_checksum = validate_insert_ranks(index, inserts);
    if (inserted != inserts.size() || deleted != inserts.size() ||
        reinserted != inserts.size() || before_checksum != after_checksum) {
        throw std::runtime_error("churn lifecycle validation failed");
    }

    std::cout << std::fixed << std::setprecision(6)
              << "{\"mode\":\"churn\",\"status\":\"success\""
              << ",\"base_keys\":" << base_count
              << ",\"churn_keys\":" << inserts.size()
              << ",\"build_ms\":" << millis(build_begin, build_end)
              << ",\"insert_ms\":" << millis(insert_begin, insert_end)
              << ",\"delete_ms\":" << millis(delete_begin, delete_end)
              << ",\"reinsert_ms\":" << millis(reinsert_begin, reinsert_end)
              << ",\"annihilation_residual_mutations\":0"
              << ",\"rank_checksum\":" << after_checksum
              << ",\"final_mutations\":" << index.mutation_count()
              << "}\n";
    return 0;
}

static void merge_base_and_inserts(const hrtli::MappedKeyStore& base,
                                   const std::vector<std::string>& inserts,
                                   const std::string& output_path) {
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create consolidated base");
    size_t i = 0;
    size_t j = 0;
    while (i < base.size() || j < inserts.size()) {
        if (j == inserts.size() ||
            (i < base.size() && base.view(i) < std::string_view(inserts[j]))) {
            const std::string_view key = base.view(i++);
            output.write(key.data(), static_cast<std::streamsize>(key.size()));
        } else if (i == base.size() ||
                   std::string_view(inserts[j]) < base.view(i)) {
            output.write(inserts[j].data(),
                         static_cast<std::streamsize>(inserts[j].size()));
            ++j;
        } else {
            throw std::runtime_error("duplicate encountered during consolidation");
        }
        output.put('\n');
    }
    if (!output) throw std::runtime_error("failed while writing consolidated base");
}

static int run_consolidation(const std::string& base_path,
                             const std::vector<std::string>& inserts,
                             size_t base_count,
                             const std::string& output_path) {
    double preconsolidation_build_ms = 0.0;
    Clock::time_point consolidation_begin;
    double merge_ms = 0.0;
    {
        const auto old_build_begin = Clock::now();
        hrtli::PackedRankTransportIndex old_index(base_path, base_count, 64, true);
        const auto old_build_end = Clock::now();
        preconsolidation_build_ms = millis(old_build_begin, old_build_end);
        for (const auto& key : inserts) {
            if (!old_index.insert(key)) throw std::runtime_error("insert rejected");
        }
        consolidation_begin = Clock::now();
        const auto merge_begin = Clock::now();
        merge_base_and_inserts(old_index.base_store(), inserts, output_path);
        const auto merge_end = Clock::now();
        merge_ms = millis(merge_begin, merge_end);
    }

    const auto rebuild_begin = Clock::now();
    hrtli::PackedRankTransportIndex rebuilt(
        output_path, base_count + inserts.size(), 64, true);
    const auto rebuild_end = Clock::now();
    const auto certify_begin = Clock::now();
    const int max_error = rebuilt.base_model_max_error();
    const auto certify_end = Clock::now();
    if (max_error > 64 || rebuilt.mutation_count() != 0) {
        throw std::runtime_error("consolidated generation failed certification");
    }

    uint64_t checksum = 0;
    const size_t stride = std::max<size_t>(1, rebuilt.base_store().size() / 100000);
    size_t checks = 0;
    for (size_t i = 0; i < rebuilt.base_store().size() && checks < 100000;
         i += stride, ++checks) {
        const std::string key(rebuilt.base_store().view(i));
        if (!rebuilt.point_lookup(key) || rebuilt.binary_exact_rank(key) != static_cast<int>(i)) {
            throw std::runtime_error("consolidated generation rank validation failed");
        }
        checksum += i;
    }
    const auto total_end = Clock::now();

    std::cout << std::fixed << std::setprecision(6)
              << "{\"mode\":\"stop_the_world_consolidation\",\"status\":\"success\""
              << ",\"old_base_keys\":" << base_count
              << ",\"mutations_absorbed\":" << inserts.size()
              << ",\"new_base_keys\":" << rebuilt.base_store().size()
              << ",\"preconsolidation_build_ms\":" << preconsolidation_build_ms
              << ",\"merge_ms\":" << merge_ms
              << ",\"rebuild_ms\":" << millis(rebuild_begin, rebuild_end)
              << ",\"certify_ms\":" << millis(certify_begin, certify_end)
              << ",\"total_ms\":" << millis(consolidation_begin, total_end)
              << ",\"certified_max_error\":" << max_error
              << ",\"model_segments\":" << rebuilt.model_segments()
              << ",\"post_consolidation_mutations\":" << rebuilt.mutation_count()
              << ",\"sampled_exact_ranks\":" << checks
              << ",\"rank_checksum\":" << checksum
              << "}\n";
    return 0;
}

int main(int argc, char** argv) {
    try {
        if (argc < 6) {
            std::cerr << "usage: benchmark_commoncrawl_200m_lifecycle "
                         "churn|consolidate BASE INSERTS BASE_COUNT INSERT_COUNT "
                         "[CONSOLIDATED_OUTPUT]\n";
            return 2;
        }
        const std::string mode = argv[1];
        const std::string base_path = argv[2];
        const std::string insert_path = argv[3];
        const size_t base_count = std::stoull(argv[4]);
        const size_t insert_count = std::stoull(argv[5]);
        const auto inserts = read_keys(insert_path, insert_count);
        if (mode == "churn") return run_churn(base_path, inserts, base_count);
        if (mode == "consolidate" && argc == 7) {
            return run_consolidation(base_path, inserts, base_count, argv[6]);
        }
        throw std::runtime_error("invalid lifecycle mode or arguments");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
