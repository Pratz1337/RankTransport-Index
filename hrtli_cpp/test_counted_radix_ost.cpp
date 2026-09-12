#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "counted_radix_ost.hpp"
#include "rank_transport.hpp"
#include "treap.hpp"

int main() {
    hrtli::CountedRadixOST ost;
    assert(ost.insert("/a"));
    assert(ost.insert("/a/b"));
    assert(ost.insert("/c"));
    assert(!ost.insert("/a"));
    assert(ost.size() == 3);
    assert(ost.lookup("/a") == 0);
    assert(ost.lookup("/a/b") == 1);
    assert(ost.lookup("/c") == 2);
    assert(ost.count_range("/a", "/b") == 2);
    assert(ost.remove("/a/b"));
    assert(ost.lookup("/c") == 1);
    assert(ost.validate_internal());

    std::vector<std::string> base = {"/a", "/c", "/e", "/g"};
    hrtli::RankTransportIndex hrt(base, 1);
    hrtli::CountedRadixOST live;
    hrtli::OrderStatisticTreap treap;
    live.bulk_load(base);
    for (const auto& key : base) assert(treap.add(key));

    assert(hrt.insert("/b"));
    assert(hrt.insert("/f"));
    assert(hrt.remove("/c"));
    assert(live.insert("/b"));
    assert(live.insert("/f"));
    assert(live.remove("/c"));
    assert(treap.add("/b"));
    assert(treap.add("/f"));
    assert(treap.discard("/c"));

    std::vector<std::string> live_keys = {"/a", "/b", "/e", "/f", "/g"};
    for (const auto& key : live_keys) {
        assert(hrt.exact_rank(key) == live.rank(key));
        assert(live.rank(key) == treap.rank(key));
        assert(hrt.lookup(key) == live.lookup(key));
    }
    assert(hrt.count_range("/a", "/f") == live.count_range("/a", "/f"));
    assert(hrt.lookup("/c") == -1);
    assert(live.lookup("/c") == -1);
    std::cout << "counted radix OST tests passed\n";
    return 0;
}
