#pragma once

#include <string>
#include <vector>

namespace hrtli::external {

struct BindingSpec {
    const char* name;
    const char* macro;
    const char* include_hint;
    const char* upstream;
    const char* license_note;
};

inline std::vector<BindingSpec> binding_specs() {
    return {
        {"ALEX", "HRTLI_WITH_ALEX", "alex.h", "https://github.com/microsoft/ALEX", "4370da6aa8b509fdc9b0d2c49faa0624b0078589; MIT"},
        {"PGM-index", "HRTLI_WITH_PGM", "pgm/pgm_index_dynamic.hpp", "https://github.com/gvinciguerra/PGM-index", "c6fcf3d34e55eb0061b01e2f49dfcbdb711f1407; Apache-2.0"},
        {"LIPP", "HRTLI_WITH_LIPP", "lipp.h", "https://github.com/Jiacheng-WU/lipp", "fe6ca4954f00875482f9e4dd63b34dae2384d23b; MIT"},
        {"ART", "HRTLI_WITH_ART", "art.h", "https://github.com/armon/libart", "301046804af165269e37da6725f5a4aec9ecc881; BSD-3-Clause-style"},
        {"HOT", "HRTLI_WITH_HOT", "hot/singlethreaded/HOTSingleThreaded.hpp", "https://github.com/speedskater/hot", "96bf6fb7103b27e50e16a6026db8974c090ee84a; ISC"},
        {"LITS", "HRTLI_WITH_LITS", "lits.hpp", "https://github.com/schencoding/lits", "c9026ac9645b4af1f6e3cb27b42351220f4376d4; no root LICENSE found"},
    };
}

}  // namespace hrtli::external

#if defined(HRTLI_WITH_ALEX)
#include "alex.h"
namespace hrtli::external {
template <class Key, class Payload>
using AlexIndex = alex::Alex<Key, Payload>;
}
#endif

#if defined(HRTLI_WITH_PGM)
#include "pgm/pgm_index_dynamic.hpp"
namespace hrtli::external {
template <class Key, class Payload>
using DynamicPgmIndex = pgm::DynamicPGMIndex<Key, Payload>;
}
#endif

#if defined(HRTLI_WITH_LIPP)
#include "lipp.h"
namespace hrtli::external {
template <class Key, class Payload>
using LippIndex = LIPP<Key, Payload>;
}
#endif

#if defined(HRTLI_WITH_ART)
#include "art.h"
#endif

#if defined(HRTLI_WITH_HOT)
#include <hot/singlethreaded/HOTSingleThreaded.hpp>
#endif

#if defined(HRTLI_WITH_LITS)
#include "lits.hpp"
#endif
