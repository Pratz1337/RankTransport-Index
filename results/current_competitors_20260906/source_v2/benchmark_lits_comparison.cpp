// Shared natural-key set workload. Competitors have no rank adapter here.
#include <chrono>
#include <climits>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sys/resource.h>
#include "packed_rank_transport.hpp"
#include "lits/lits.hpp"
#ifdef HRTLI_WITH_ART_HOT
#include "art.h"
#include "hot/singlethreaded/HOTSingleThreaded.hpp"
#include "idx/contenthelpers/IdentityKeyExtractor.hpp"
#endif

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Read-only, NUL-delimited copy of the same newline-delimited natural keys.
// This common harness input is resident/mapped overhead, not index-only memory.
class Corpus {
    int fd_ = -1;
    const char* bytes_ = nullptr;
    size_t length_ = 0;
    std::vector<size_t> offsets_;
    void release() {
        if (bytes_) munmap(const_cast<char*>(bytes_), length_);
        if (fd_ >= 0) close(fd_);
    }
public:
    Corpus(const char* path, size_t count) {
        try {
            fd_ = open(path, O_RDONLY);
            require(fd_ >= 0, "cannot open NUL corpus");
            struct stat info {};
            require(fstat(fd_, &info) == 0 && info.st_size > 0, "empty or invalid corpus");
            length_ = static_cast<size_t>(info.st_size);
            void* mapped = mmap(nullptr, length_, PROT_READ, MAP_PRIVATE, fd_, 0);
            require(mapped != MAP_FAILED, "cannot map corpus");
            bytes_ = static_cast<const char*>(mapped);
            offsets_.reserve(count + 1);
            offsets_.push_back(0);
            for (size_t i = 0; i < length_; ++i) {
                unsigned char c = bytes_[i];
                require(c < 128 && c != '\n' && c != '\r', "non-host ASCII corpus byte");
                if (c == 0) {
                    require(i > offsets_.back(), "empty corpus key");
                    offsets_.push_back(i + 1);
                }
            }
            require(offsets_.size() == count + 1 && offsets_.back() == length_, "corpus count/terminator mismatch");
            for (size_t i = 1; i < count; ++i)
                require(view(i - 1) < view(i), "corpus not sorted and unique");
        } catch (...) { release(); throw; }
    }
    Corpus(const Corpus&) = delete;
    Corpus& operator=(const Corpus&) = delete;
    ~Corpus() { release(); }
    size_t size() const { return offsets_.size() - 1; }
    const char* c_str(size_t i) const { return bytes_ + offsets_[i]; }
    std::string_view view(size_t i) const {
        return {c_str(i), offsets_[i + 1] - offsets_[i] - 1};
    }
};

struct Operation {
    std::string key; // Identical, stable pre-materialized strings for every API.
    uint64_t id;
    uint8_t type; // 0 membership, 1 insert, 2 delete.
    uint8_t expected;
};

struct HrtliAdapter {
    hrtli::PackedRankTransportIndex index;
    HrtliAdapter(const Corpus& base, const char* path, int epsilon)
        : index(path, base.size(), epsilon, false) {}
    bool contains(const std::string& key) { return index.point_lookup(key); }
    bool insert(const std::string& key) { return index.insert(key); }
    bool erase(const std::string& key) { return index.remove(key); }
};

struct LitsAdapter {
    lits::LITS index;
    LitsAdapter(const Corpus& base, const char*, int) {
        std::vector<const char*> pointers(base.size());
        std::vector<uint64_t> values(base.size(), 1);
        for (size_t i = 0; i < base.size(); ++i) pointers[i] = base.c_str(i);
        require(index.bulkload(pointers.data(), values.data(), int(base.size())), "LITS bulkload rejected");
    }
    // Upstream destruction is explicit; measure it separately from workload time.
    ~LitsAdapter() { index.destroy(); }
    bool contains(const std::string& key) { return index.lookup(key.c_str()) != nullptr; }
    bool insert(const std::string& key) { return index.insert(key.c_str(), 1); }
    bool erase(const std::string& key) { return index.remove(key.c_str()); }
};

#ifdef HRTLI_WITH_ART_HOT
struct ArtAdapter {
    art_tree index {};
    char present = 1; // Non-null set payload, owned for the tree's lifetime.
    ArtAdapter(const Corpus& base, const char*, int) {
        require(art_tree_init(&index) == 0, "ART init failed");
        for (size_t i = 0; i < base.size(); ++i)
            require(art_insert_no_replace(&index,
                reinterpret_cast<const unsigned char*>(base.c_str(i)),
                int(base.view(i).size() + 1), &present) == nullptr, "ART build duplicate");
    }
    ~ArtAdapter() { art_tree_destroy(&index); }
    bool contains(const std::string& key) {
        return art_search(&index, reinterpret_cast<const unsigned char*>(key.c_str()), int(key.size() + 1)) != nullptr;
    }
    bool insert(const std::string& key) {
        return art_insert_no_replace(&index, reinterpret_cast<const unsigned char*>(key.c_str()),
                                     int(key.size() + 1), &present) == nullptr;
    }
    bool erase(const std::string& key) {
        return art_delete(&index, reinterpret_cast<const unsigned char*>(key.c_str()), int(key.size() + 1)) != nullptr;
    }
};

struct HotAdapter {
    hot::singlethreaded::HOTSingleThreaded<const char*, idx::contenthelpers::IdentityKeyExtractor> index;
    HotAdapter(const Corpus& base, const char*, int) {
        for (size_t i = 0; i < base.size(); ++i)
            require(index.insert(base.c_str(i)), "HOT build duplicate");
    }
    // HOT borrows keys. Corpus mappings and every operation string remain alive
    // and immutable until after this adapter is destroyed; HOT owns its nodes.
    bool contains(const std::string& key) { return index.lookup(key.c_str()).mIsValid; }
    bool insert(const std::string& key) { return index.insert(key.c_str()); }
    bool erase(const std::string& key) { return index.remove(key.c_str()); }
};
#endif

using Clock = std::chrono::steady_clock;
static double seconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double>(end - start).count();
}

template<class Adapter>
static void run(const char* mode, const Corpus& base, const Corpus& arrivals,
                const char* path, int epsilon, const std::vector<Operation>& operations,
                const std::vector<uint8_t>& final_live, uint64_t seed, size_t write_percent,
                uint64_t trace_hash, const std::string& family) {
    size_t max_key_bytes = 0;
    for (const Corpus* corpus : {&base, &arrivals})
        for (size_t i = 0; i < corpus->size(); ++i)
            max_key_bytes = std::max(max_key_bytes, corpus->view(i).size());
#ifdef HRTLI_WITH_ART_HOT
    // Reject the entire experiment, for every mode, rather than silently
    // truncating keys or selecting a different population for HOT.
    require(max_key_bytes < idx::contenthelpers::MAX_STRING_KEY_LENGTH, "key exceeds common HOT domain");
#endif
    const auto build_start = Clock::now();
    Adapter index(base, path, epsilon);
    const auto build_end = Clock::now();
    auto key_at = [&](size_t id) {
        return std::string(id < base.size() ? base.view(id) : arrivals.view(id - base.size()));
    };
    for (size_t i = 0; i < final_live.size(); ++i)
        require(index.contains(key_at(i)) == (i < base.size()), "initial state differs from oracle");
    for (size_t i = 0; i < std::min<size_t>(operations.size(), 10000); ++i)
        require(index.contains(operations[i].key) == (operations[i].id < base.size()), "warmup mismatch");
    std::vector<uint8_t> answers(operations.size());
    struct rusage before_workload {}, after_workload {};
    require(getrusage(RUSAGE_SELF, &before_workload) == 0, "cannot read workload resources");
    const double started_unix = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto start = Clock::now();
    for (size_t i = 0; i < operations.size(); ++i) {
        const auto& op = operations[i];
        answers[i] = op.type == 0 ? index.contains(op.key) :
                     op.type == 1 ? index.insert(op.key) : index.erase(op.key);
    }
    const auto end = Clock::now();
    const double ended_unix = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    require(getrusage(RUSAGE_SELF, &after_workload) == 0, "cannot read workload resources");
    size_t counts[3] = {}, positive[3] = {};
    for (size_t i = 0; i < operations.size(); ++i) {
        require(answers[i] == operations[i].expected, "timed answer differs from independent oracle");
        ++counts[operations[i].type];
        positive[operations[i].type] += answers[i];
    }
    size_t live_count = 0;
    for (size_t i = 0; i < final_live.size(); ++i) {
        require(index.contains(key_at(i)) == bool(final_live[i]), "final state differs from oracle");
        live_count += final_live[i];
    }
    struct rusage usage {};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "cannot read resource usage");
    // Emit after all correctness checks; the enclosing recorder also checks exit status.
    std::cout << std::setprecision(12)
        << "{\"mode\":\"" << mode << "\",\"base_keys\":" << base.size()
        << ",\"workload_family\":\"" << family << "\""
        << ",\"arrival_keys\":" << arrivals.size() << ",\"operations\":" << operations.size()
        << ",\"max_key_bytes\":" << max_key_bytes
        << ",\"write_percent\":" << write_percent << ",\"seed\":" << seed
        << ",\"epsilon\":" << epsilon << ",\"trace_fnv64\":\"" << trace_hash << "\""
        << ",\"build_seconds\":" << seconds(build_start, build_end)
        << ",\"workload_seconds\":" << seconds(start, end)
        << ",\"workload_started_unix\":" << started_unix << ",\"workload_ended_unix\":" << ended_unix
        << ",\"workload_major_faults\":" << after_workload.ru_majflt - before_workload.ru_majflt
        << ",\"workload_minor_faults\":" << after_workload.ru_minflt - before_workload.ru_minflt
        << ",\"reads\":" << counts[0] << ",\"read_hits\":" << positive[0]
        << ",\"insert_attempts\":" << counts[1] << ",\"insert_successes\":" << positive[1]
        << ",\"delete_attempts\":" << counts[2] << ",\"delete_successes\":" << positive[2]
        << ",\"final_live_keys\":" << live_count << ",\"max_rss_kib\":" << usage.ru_maxrss
        << ",\"rss_scope\":\"whole process including shared corpus, trace, oracle and build temporaries\""
        << ",\"all_answers_checked\":true,\"initial_and_final_states_checked\":true}\n";
}

#ifndef HRTLI_COMPARISON_NO_MAIN
int main(int argc, char** argv) {
    try {
        require(argc == 11 || argc == 12, "usage: comparison MODE BASE_TXT BASE_NUL ARRIVALS_NUL BASE_N ARRIVAL_N OPS WRITE_PERCENT SEED EPSILON [attempts|valid_writes]");
        std::string mode = argv[1];
        require(mode == "hrtli" || mode == "lits"
#ifdef HRTLI_WITH_ART_HOT
                || mode == "art" || mode == "hot"
#endif
                , "unknown mode");
        std::string family = argc == 12 ? argv[11] : "attempts";
        require(family == "attempts" || family == "valid_writes", "unknown workload family");
        size_t base_count = std::stoull(argv[5]), arrival_count = std::stoull(argv[6]);
        size_t operation_count = std::stoull(argv[7]), write_percent = std::stoull(argv[8]);
        uint64_t seed = std::stoull(argv[9]);
        int epsilon = std::stoi(argv[10]);
        require(base_count >= 1000 && arrival_count > 0 && base_count <= INT_MAX &&
                arrival_count <= size_t(INT_MAX) - base_count, "cardinality outside shared API domain");
        require(operation_count > 0 && operation_count <= SIZE_MAX / 100 && write_percent <= 100 && epsilon >= 0,
                "invalid operation count, write percentage or epsilon");
        Corpus base(argv[3], base_count), arrivals(argv[4], arrival_count);
        {
            hrtli::MappedKeyStore text_base(argv[2], base_count);
            for (size_t i = 0; i < base_count; ++i)
                require(text_base.view(i) == base.view(i), "TXT/NUL base differs");
        }
        size_t cursor = 0;
        for (size_t i = 0; i < arrival_count; ++i) {
            while (cursor < base_count && base.view(cursor) < arrivals.view(i)) ++cursor;
            require(cursor == base_count || base.view(cursor) != arrivals.view(i), "base/arrival overlap");
        }
        std::mt19937_64 random(seed);
        std::vector<Operation> operations;
        operations.reserve(operation_count);
        size_t writes = operation_count * write_percent / 100;
        for (size_t i = 0; i < operation_count; ++i) {
            size_t id = random() % (base_count + arrival_count);
            uint8_t type = i < writes ? uint8_t(1 + random() % 2) : 0;
            operations.push_back({std::string(id < base_count ? base.view(id) : arrivals.view(id - base_count)), id, type, 0});
        }
        std::shuffle(operations.begin(), operations.end(), random);
        if (family == "valid_writes") {
            // Choose from independent eligible sets before either index exists.
            // Alternating successful signs hold live cardinality within one key.
            // These preparation-only pools are released before index construction.
            std::vector<size_t> active(base_count), inactive(arrival_count);
            inactive.reserve(arrival_count + 1);
            std::iota(active.begin(), active.end(), 0);
            std::iota(inactive.begin(), inactive.end(), base_count);
            bool deleting = true;
            for (auto& op : operations) if (op.type) {
                auto& from = deleting ? active : inactive;
                auto& to = deleting ? inactive : active;
                size_t slot = random() % from.size();
                op.id = from[slot];
                from[slot] = from.back();
                from.pop_back();
                to.push_back(op.id);
                op.type = deleting ? 2 : 1;
                op.key = std::string(op.id < base_count ? base.view(op.id) : arrivals.view(op.id - base_count));
                deleting = !deleting;
            }
        }
        std::vector<uint8_t> live(base_count + arrival_count, 0);
        std::fill(live.begin(), live.begin() + base_count, 1);
        uint64_t hash = 14695981039346656037ULL;
        for (auto& op : operations) {
            op.expected = op.type == 1 ? !live[op.id] : live[op.id];
            require(family != "valid_writes" || op.type == 0 || op.expected,
                    "eligible-set generator produced an invalid write");
            if (op.type) live[op.id] = op.type == 1;
            // Canonical little-endian integer bytes; supplementary trace equality only.
            for (unsigned shift = 0; shift < 64; shift += 8)
                hash = (hash ^ uint8_t(op.id >> shift)) * 1099511628211ULL;
            hash = (hash ^ op.type) * 1099511628211ULL;
            hash = (hash ^ op.expected) * 1099511628211ULL;
        }
        if (mode == "hrtli") run<HrtliAdapter>(argv[1], base, arrivals, argv[2], epsilon, operations, live, seed, write_percent, hash, family);
        else if (mode == "lits") run<LitsAdapter>(argv[1], base, arrivals, argv[2], epsilon, operations, live, seed, write_percent, hash, family);
#ifdef HRTLI_WITH_ART_HOT
        else if (mode == "art") run<ArtAdapter>(argv[1], base, arrivals, argv[2], epsilon, operations, live, seed, write_percent, hash, family);
        else run<HotAdapter>(argv[1], base, arrivals, argv[2], epsilon, operations, live, seed, write_percent, hash, family);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
#endif
