#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "packed_rank_transport.hpp"

using Clock = std::chrono::steady_clock;

static double millis(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "usage: benchmark_commoncrawl_200m_epsilon BASE BASE_COUNT\n";
            return 2;
        }
        hrtli::MappedKeyStore base(argv[1], std::stoull(argv[2]));
        if (!base.strictly_sorted_unique()) {
            throw std::runtime_error("base is not strictly sorted and unique");
        }
        const int epsilons[] = {16, 32, 64, 128};
        std::cout << "{\"dataset_keys\":" << base.size() << ",\"rows\":[";
        bool first = true;
        for (const int epsilon : epsilons) {
            hrtli::PackedPiecewiseModel model;
            const auto build_begin = Clock::now();
            model.build(base, epsilon);
            const auto build_end = Clock::now();
            const auto certify_begin = Clock::now();
            const int max_error = model.max_error(base);
            const auto certify_end = Clock::now();
            if (!first) std::cout << ',';
            first = false;
            std::cout << std::fixed << std::setprecision(6)
                      << "{\"epsilon\":" << epsilon
                      << ",\"segments\":" << model.size()
                      << ",\"repaired_segments\":" << model.repaired_segments()
                      << ",\"certified_max_error\":" << max_error
                      << ",\"bound_holds\":" << (max_error <= epsilon ? "true" : "false")
                      << ",\"build_ms\":" << millis(build_begin, build_end)
                      << ",\"certify_ms\":" << millis(certify_begin, certify_end)
                      << '}';
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
