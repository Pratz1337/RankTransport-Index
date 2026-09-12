// Real-corpus, stop-the-world signed-ledger materialization and rebuild audit.
// The output must be a new path. No generation is published automatically.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sys/resource.h>

#include "packed_rank_transport.hpp"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    try {
        if (argc != 7) {
            std::cerr << "usage: benchmark_signed_consolidation BASE INSERT BASE_COUNT STRIDE EPS NEW_OUTPUT\n";
            return 2;
        }
        const size_t base_count = std::stoull(argv[3]);
        const size_t stride = std::stoull(argv[4]);
        const int epsilon = std::stoi(argv[5]);
        require(stride > 2 && base_count > 0 && base_count % (stride - 1) == 0,
                "unsupported interleaved split");
        const size_t changes = base_count / (stride - 1);
        std::ifstream input(argv[2], std::ios::binary);
        require(static_cast<bool>(input), "cannot open insertion file");
        std::vector<std::string> inserts;
        for (std::string key; std::getline(input, key);) inserts.push_back(key);
        require(inserts.size() == changes, "insertion cardinality mismatch");
        double build_ms, insert_ms, delete_ms, merge_ms;
        int transported_error = 0;
        size_t transported_checks = 0;
        size_t emitted = 0;
        Clock::time_point transition;
        {
            auto start = Clock::now();
            hrtli::PackedRankTransportIndex index(argv[1], base_count, epsilon, false);
            build_ms = ms(start);
            const auto& base = index.base_store();
            // Verify source positions independently before using them as ranks.
            for (size_t j = 0; j < changes; ++j) {
                const size_t pos = (j + 1) * (stride - 1);
                require(base.view(pos - 1) < inserts[j] &&
                        (pos == base_count || inserts[j] < base.view(pos)),
                        "source interleaving mismatch");
            }
            std::vector<size_t> order(changes);
            std::iota(order.begin(), order.end(), 0);
            std::mt19937_64 rng(20260905);
            std::shuffle(order.begin(), order.end(), rng);
            std::cerr << "built certified hash-free base; applying signed mutations\n";
            start = Clock::now();
            for (size_t j : order) require(index.insert(inserts[j]), "insert failed");
            insert_ms = ms(start);
            // Delete a contiguous leading region, not one key beside each
            // insertion: the signed correction now has substantial rank drift.
            std::vector<std::string> deletes;
            for (size_t j = 0; j < changes; ++j) deletes.emplace_back(base.view(j));
            start = Clock::now();
            for (size_t j : order) require(index.remove(deletes[j]), "base delete failed");
            delete_ms = ms(start);
            require(index.mutation_count() == 2 * changes, "signed ledger cardinality mismatch");
            const size_t step = std::max<size_t>(1, (base_count - changes) / 100000);
            for (size_t pos = changes; pos < base_count && transported_checks < 100000;
                 pos += step, ++transported_checks) {
                const std::string key(base.view(pos));
                const int expected = static_cast<int>(pos + pos / (stride - 1) - changes);
                require(index.learned_exact_rank(key) == expected, "pre-rebuild live rank mismatch");
                transported_error = std::max(transported_error,
                    std::abs(index.transported_predict(key) - expected));
            }
            require(transported_error <= epsilon, "signed-state transported certificate failed");
            transition = Clock::now();
            const int fd = ::open(argv[6], O_WRONLY | O_CREAT | O_EXCL, 0600);
            require(fd >= 0, "output must be a new writable file (existing files are preserved)");
            FILE* output = ::fdopen(fd, "wb");
            if (!output) { ::close(fd); throw std::runtime_error("cannot open output stream"); }
            std::vector<char> buffer(1024 * 1024);
            ::setvbuf(output, buffer.data(), _IOFBF, buffer.size());
            std::cerr << "materializing live ledger; checking every emitted key against source merge\n";
            size_t b = changes, j = 0;
            try {
                emitted = index.for_each_live([&](std::string_view actual) {
                    // Independent oracle: original input vectors plus explicit
                    // deleted prefix, not the ledger's enumerated mutation lists.
                    require(b < base_count || j < changes, "extra materialized key");
                    std::string_view expected;
                    if (j < changes && (b == base_count || std::string_view(inserts[j]) < base.view(b)))
                        expected = inserts[j++];
                    else expected = base.view(b++);
                    require(actual == expected, "materialized key differs from independent live-set oracle");
                    require(::fwrite(actual.data(), 1, actual.size(), output) == actual.size() &&
                            ::fputc('\n', output) != EOF, "generation write failed");
                });
                require(b == base_count && j == changes && emitted == base_count,
                        "materialization omitted live keys");
                require(::fflush(output) == 0 && ::fsync(fd) == 0, "generation flush failed");
            } catch (...) { ::fclose(output); throw; }
            require(::fclose(output) == 0, "generation close failed");
            merge_ms = ms(transition);
        }
        // Old index is released to fit the guest memory budget. This is an
        // offline transition, not an atomic, available, crash-safe replacement.
        std::cerr << "materialized exact live set; rebuilding hash-free generation\n";
        auto start = Clock::now();
        hrtli::PackedRankTransportIndex rebuilt(argv[6], emitted, epsilon, false);
        const double rebuild_ms = ms(start);
        start = Clock::now();
        const int error = rebuilt.base_model_max_error();
        const double certify_ms = ms(start);
        require(error <= epsilon && rebuilt.mutation_count() == 0, "rebuilt generation certificate failed");
        size_t rank_checks = 0;
        const size_t step = std::max<size_t>(1, emitted / 100000);
        for (size_t pos = 0; pos < emitted && rank_checks < 100000; pos += step, ++rank_checks) {
            const std::string key(rebuilt.base_store().view(pos));
            require(rebuilt.learned_point_lookup(key) && rebuilt.learned_exact_rank(key) == static_cast<int>(pos),
                    "rebuilt membership/rank mismatch");
        }
        const double total_ms = ms(transition);
        rusage usage{};
        require(::getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
        std::cout << std::setprecision(12)
                  << "{\"mode\":\"signed_stop_the_world_consolidation\",\"fingerprints\":false"
                  << ",\"base_keys\":" << base_count << ",\"insertions\":" << changes
                  << ",\"base_deletions\":" << changes << ",\"materialized_keys_checked\":" << emitted
                  << ",\"epsilon\":" << epsilon << ",\"initial_build_ms\":" << build_ms
                  << ",\"insert_ms\":" << insert_ms << ",\"base_delete_ms\":" << delete_ms
                  << ",\"merge_write_flush_ms\":" << merge_ms << ",\"rebuild_ms\":" << rebuild_ms
                  << ",\"certify_ms\":" << certify_ms << ",\"transition_total_ms\":" << total_ms
                  << ",\"transported_checks\":" << transported_checks
                  << ",\"transported_max_error\":" << transported_error
                  << ",\"rebuilt_max_error\":" << error << ",\"rebuilt_rank_checks\":" << rank_checks
                  << ",\"peak_rss_kib\":" << usage.ru_maxrss
                  << ",\"post_rebuild_mutations\":0,\"all_oracles_passed\":true}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
