// Adversarial correctness fixtures, never used as real-corpus benchmark data.
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include "packed_rank_transport.hpp"

struct Fixture {
    std::string path;
    explicit Fixture(const std::vector<std::string>& keys) {
        char name[] = "/tmp/hrtli_certificate_XXXXXX";
        int fd = mkstemp(name);
        if (fd < 0) throw std::runtime_error("mkstemp failed");
        close(fd);
        path = name;
        std::ofstream out(path, std::ios::binary);
        for (const auto& key : keys) out << key << '\n';
    }
    ~Fixture() { unlink(path.c_str()); }
};

int main() {
    try {
        std::mt19937_64 rng(20260905);
        std::set<std::string> unique = {"", "a", "aa", "aaa", "b",
            std::string(1, '\0'), std::string(1, '\x7f'),
            std::string(1, '\x80'), std::string(1, '\xff')};
        for (size_t i = 0; i < 12000; ++i) {
            std::string key(i % 3 == 0 ? "com.shared.long.prefix." : "");
            for (size_t j = 0, n = 1 + rng() % 48; j < n; ++j) {
                unsigned char byte = static_cast<unsigned char>(rng());
                if (byte == '\n') byte = 11; // newline is the corpus delimiter
                key.push_back(static_cast<char>(byte));
            }
            unique.insert(key);
        }
        std::vector<std::string> keys(unique.begin(), unique.end());
        Fixture fixture(keys);
        hrtli::MappedKeyStore base(fixture.path, keys.size());
        for (int epsilon : {0, 1, 2, 4, 16, 32, 64, 128}) {
            hrtli::PackedPiecewiseModel model;
            model.build(base, epsilon);
            int realized = model.max_error(base);
            std::cout << "epsilon=" << epsilon << " realized=" << realized
                      << " segments=" << model.size() << std::endl;
            if (realized > epsilon) throw std::runtime_error("uncertified model accepted");
            for (size_t i = 0; i < keys.size(); ++i) {
                if (std::abs(model.predict(keys[i], base) - static_cast<int>(i)) > epsilon)
                    throw std::runtime_error("query routing disagrees with certificate");
                bool fallback = true;
                if (model.lower_bound(keys[i], base, &fallback) != i || fallback)
                    throw std::runtime_error("certified stored key missed its bounded window");
                std::string absent = keys[i] + std::string(1, '\0');
                size_t expected = std::lower_bound(keys.begin(), keys.end(), absent) - keys.begin();
                if (model.lower_bound(absent, base) != expected)
                    throw std::runtime_error("absent boundary lower bound mismatch");
            }
        }
        hrtli::PackedPiecewiseModel invalid;
        bool rejected = false;
        try { invalid.build(base, -1); } catch (const std::invalid_argument&) { rejected = true; }
        if (!rejected) throw std::runtime_error("negative epsilon accepted");
        {
            Fixture empty({});
            hrtli::PackedRankTransportIndex index(empty.path, 0, 0, false);
            if (index.point_lookup("absent") || index.learned_count_range("", "z") != 0)
                throw std::runtime_error("empty index query mismatch");
            if (!index.insert("") || index.learned_exact_rank("") != 0 || !index.remove(""))
                throw std::runtime_error("empty-key lifecycle mismatch");
        }
        // Independent sorted-vector oracle covers failed writes, negative ledger
        // weights, annihilation, exact ranks and arbitrary inclusive boundaries.
        std::vector<std::string> initial(keys.begin(), keys.begin() + 600);
        Fixture dynamic_fixture(initial);
        hrtli::PackedRankTransportIndex index(dynamic_fixture.path, initial.size(), 4, false);
        std::set<std::string> live(initial.begin(), initial.end());
        for (size_t op = 0; op < 2400; ++op) {
            const auto& key = keys[rng() % 1200];
            if (rng() & 1) {
                const bool expected = live.insert(key).second;
                if (index.insert(key) != expected) throw std::runtime_error("insert result mismatch");
            } else {
                const bool expected = live.erase(key) != 0;
                if (index.remove(key) != expected) throw std::runtime_error("delete result mismatch");
            }
            if (op % 40) continue;
            std::vector<std::string> sorted(live.begin(), live.end());
            std::vector<std::string> materialized;
            const size_t written = index.for_each_live([&](std::string_view key) {
                materialized.emplace_back(key);
            });
            if (written != sorted.size() || materialized != sorted)
                throw std::runtime_error("signed generation materialization mismatch");
            for (size_t j = 0; j < 1200; ++j) {
                const auto& probe = keys[j];
                auto where = std::lower_bound(sorted.begin(), sorted.end(), probe);
                bool exists = where != sorted.end() && *where == probe;
                if (index.learned_point_lookup(probe) != exists || index.point_lookup(probe) != exists)
                    throw std::runtime_error("membership mismatch after mixed writes");
                if (exists && index.learned_exact_rank(probe) != where - sorted.begin())
                    throw std::runtime_error("rank mismatch after mixed writes");
                if (exists && std::binary_search(initial.begin(), initial.end(), probe) &&
                    std::abs(index.transported_predict(probe) - static_cast<int>(where - sorted.begin())) > 4)
                    throw std::runtime_error("transported certificate mismatch");
                const auto& upper = keys[rng() % 1200];
                auto left = std::lower_bound(sorted.begin(), sorted.end(), probe);
                auto right = std::upper_bound(sorted.begin(), sorted.end(), upper);
                int expected = probe > upper ? 0 : static_cast<int>(right - left);
                if (index.learned_count_range(probe, upper) != expected)
                    throw std::runtime_error("range mismatch after mixed writes");
            }
        }
        std::vector<std::string> materialized;
        index.for_each_live([&](std::string_view key) { materialized.emplace_back(key); });
        Fixture consolidated(materialized);
        hrtli::PackedRankTransportIndex rebuilt(consolidated.path, materialized.size(), 4, false);
        if (rebuilt.mutation_count() != 0 || rebuilt.base_model_max_error() > 4)
            throw std::runtime_error("rebuilt generation failed certificate/reset");
        for (size_t i = 0; i < materialized.size(); ++i) {
            if (rebuilt.learned_exact_rank(materialized[i]) != static_cast<int>(i))
                throw std::runtime_error("rebuilt rank mismatch");
        }
        const size_t mutations_before = index.mutation_count();
        try {
            index.for_each_live([](std::string_view) { throw std::runtime_error("writer failure"); });
        } catch (const std::runtime_error&) {}
        materialized.clear();
        index.for_each_live([&](std::string_view key) { materialized.emplace_back(key); });
        if (index.mutation_count() != mutations_before ||
            materialized != std::vector<std::string>(live.begin(), live.end()))
            throw std::runtime_error("failed materialization changed live state");
        for (const auto& key : live) {
            if (!index.remove(key)) throw std::runtime_error("delete-all failed");
        }
        if (index.for_each_live([](std::string_view) {
                throw std::runtime_error("empty live generation emitted a key");
            }) != 0) throw std::runtime_error("empty live generation count mismatch");
        std::cout << "PASS certification, byte routing, absent boundaries, empty state, 2400 mixed mutations, signed materialization and rebuild\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
