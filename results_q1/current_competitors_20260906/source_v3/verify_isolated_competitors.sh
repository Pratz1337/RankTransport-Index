#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_competitors_20260906
build=$(< "$out/build_path.txt")
test "$build" = /tmp/hrtli_current_competitors_Pmjw3A
mkdir "$out/source_v3"
cp hrtli_cpp/{benchmark_lits_comparison.cpp,test_competitor_set_adapters.cpp,packed_rank_transport.hpp,prefix_radix_delta.hpp,approx_local_delta.hpp} "$out/source_v3/"
cp scripts/verify_isolated_competitors.sh "$out/source_v3/"
includes=(-I "$out/source_v3" -I "$build/libart/src"
    -I "$build/hot/libs/hot/single-threaded/include"
    -I "$build/hot/libs/hot/commons/include" -I "$build/hot/libs/idx/content-helpers/include")
flags=(-O1 -g0 -march=native -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
g++ -std=c++17 "${flags[@]}" -DHRTLI_WITH_ART_HOT "${includes[@]}" \
    -MMD -MF "$out/art_hot_dependencies.d" "$out/source_v3/test_competitor_set_adapters.cpp" \
    "$build/art_sanitized.o" -o "$build/adapters_isolated_art_hot"
! grep -q '/LITS/' "$out/art_hot_dependencies.d"
for mode in hrtli art hot; do
    python3 scripts/record_experiment.py --output "$out/adapters_isolated_$mode.json" \
        --source "$out/source_v3/benchmark_lits_comparison.cpp" --source "$out/source_v3/test_competitor_set_adapters.cpp" \
        --source "$out/source_v3/packed_rank_transport.hpp" --source "$out/source_v3/prefix_radix_delta.hpp" \
        --source "$out/art_hot_dependencies.d" --source "$build/adapters_isolated_art_hot" \
        -- "$build/adapters_isolated_art_hot" "$build" "$mode"
done
