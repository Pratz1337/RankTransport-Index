#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "packed_rank_transport.hpp"

int main() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "hrtli_packed_annihilation.txt";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "a.example\n" << "c.example\n" << "e.example\n";
    }

    hrtli::PackedRankTransportIndex index(path.string(), 3, 1);
    assert(index.insert("b.example"));
    assert(index.mutation_count() == 1);
    assert(index.point_lookup("b.example"));
    assert(index.exact_rank("b.example") == 1);

    assert(index.remove("b.example"));
    assert(index.mutation_count() == 0);
    assert(!index.point_lookup("b.example"));
    assert(!index.remove("b.example"));

    assert(index.remove("c.example"));
    assert(index.mutation_count() == 1);
    assert(!index.point_lookup("c.example"));
    assert(index.insert("c.example"));
    assert(index.mutation_count() == 0);
    assert(index.point_lookup("c.example"));

    std::filesystem::remove(path);
    return 0;
}
