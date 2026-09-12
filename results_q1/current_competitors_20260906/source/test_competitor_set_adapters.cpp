#define HRTLI_COMPARISON_NO_MAIN
#include "benchmark_lits_comparison.cpp"
#include <fstream>
#include <set>

template<class Adapter>
static void check(const char* name, const Corpus& base, const char* path,
                  const std::vector<std::string>& keys) {
    Adapter index(base, path, 64);
    std::set<std::string> oracle;
    for (size_t i = 0; i < base.size(); ++i) oracle.emplace(base.view(i));
    auto verify = [&] {
        for (const auto& key : keys)
            require(index.contains(key) == bool(oracle.count(key)), "adapter membership differs from set oracle");
    };
    verify();
    std::mt19937_64 random(20260906);
    for (size_t step = 0; step < 30000; ++step) {
        const auto& key = keys[random() % keys.size()];
        switch (random() % 3) {
        case 0: require(index.contains(key) == bool(oracle.count(key)), "membership mismatch"); break;
        case 1: require(index.insert(key) == oracle.insert(key).second, "insert/duplicate mismatch"); break;
        case 2: require(index.erase(key) == bool(oracle.erase(key)), "delete/absent mismatch"); break;
        }
        if (step % 500 == 0) verify();
    }
    verify();
    for (const auto& key : keys) {
        require(index.erase(key) == bool(oracle.erase(key)), "delete-to-empty mismatch");
        require(!index.erase(key), "repeated deletion succeeded");
    }
    verify();
    for (const auto& key : keys) {
        require(index.insert(key) && oracle.insert(key).second, "reinsert after empty failed");
        require(!index.insert(key), "duplicate insert succeeded");
    }
    verify();
    std::cout << name << " PASS: " << keys.size() << " prefix/ASCII/254-byte keys, 30000 mixed operations, full-state checks\n";
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: adapter_test OUTPUT_DIRECTORY");
        std::vector<std::string> keys {"a", "aa", "aaa", "com", "com.example", "com.example.a"};
        for (int c = 1; c < 128; ++c) if (c != '\n' && c != '\r') {
            keys.push_back(std::string(1, char(c)));
            keys.push_back(std::string(1, char(c)) + "prefix");
            keys.push_back(std::string(100, 'q') + char(c));
            keys.push_back(std::string(253, 'z') + char(c));
        }
        for (size_t i = 0; i < 5000; ++i) {
            std::string key = "org.host" + std::to_string(i);
            keys.push_back(key);
            if (i % 10 == 0) keys.push_back(key + ".child");
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        std::string path = std::string(argv[1]) + "/adapter_base.txt";
        std::string nul = std::string(argv[1]) + "/adapter_base.nul";
        std::ofstream text_file(path, std::ios::binary), nul_file(nul, std::ios::binary);
        require(bool(text_file) && bool(nul_file), "cannot create test fixtures");
        size_t count = 0;
        for (size_t i = 0; i < keys.size(); i += 2) {
            text_file << keys[i] << '\n';
            nul_file << keys[i] << '\0';
            ++count;
        }
        text_file.close(); nul_file.close();
        Corpus base(nul.c_str(), count);
        check<HrtliAdapter>("HRT-LI", base, path.c_str(), keys);
        check<ArtAdapter>("ART", base, path.c_str(), keys);
        check<HotAdapter>("HOT", base, path.c_str(), keys);
        check<LitsAdapter>("LITS", base, path.c_str(), keys);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
