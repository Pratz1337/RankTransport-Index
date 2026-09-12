// Verify that a failed mapped-store constructor leaks neither fd nor mapping.
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <new>
#include "packed_rank_transport.hpp"

static bool fail_allocation = false;
void* operator new(std::size_t bytes) {
    if (fail_allocation) throw std::bad_alloc();
    if (void* memory = std::malloc(bytes ? bytes : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

static int open_descriptors() {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) throw std::runtime_error("cannot inspect file descriptors");
    int count = 0;
    while (readdir(directory)) ++count;
    closedir(directory);
    return count;
}

static bool mapped(const std::string& path) {
    std::ifstream maps("/proc/self/maps");
    if (!maps) throw std::runtime_error("cannot inspect memory mappings");
    for (std::string line; std::getline(maps, line);)
        if (line.find(path) != std::string::npos) return true;
    return false;
}

int main() {
    char filename[] = "/tmp/hrtli_mapped_failure_XXXXXX";
    const int fd = mkstemp(filename);
    if (fd < 0) return 1;
    const std::string path(filename);
    const bool written = write(fd, "a\nb\n", 4) == 4;
    close(fd);
    if (!written) { unlink(filename); return 1; }
    try {
        for (size_t keys : {size_t(2), size_t(0)}) {
            if (keys == 0 && truncate(filename, 0) != 0)
                throw std::runtime_error("cannot prepare empty fixture");
            const int before = open_descriptors();
            bool rejected = false;
            fail_allocation = true;
            try { hrtli::MappedKeyStore store(path, keys); }
            catch (const std::bad_alloc&) { rejected = true; }
            fail_allocation = false;
            if (!rejected || mapped(path) || open_descriptors() != before)
                throw std::runtime_error("failed constructor leaked a descriptor or mapping");
            hrtli::MappedKeyStore recovered(path, keys);
            if (recovered.size() != keys) throw std::runtime_error("recovery construction failed");
        }
        unlink(filename);
        std::cout << "PASS: nonempty and empty mapped-store allocation failures release resources\n";
    } catch (const std::exception& error) {
        fail_allocation = false;
        unlink(filename);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
