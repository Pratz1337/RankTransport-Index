#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_competitors_20260906
build=$(< "$out/build_path.txt")
test "$build" = /tmp/hrtli_current_competitors_Pmjw3A
(cd "$build/LITS" && git apply "$workspace/scripts/patches/lits-zero-prefix-context.patch")
mkdir "$out/source_v2"
cp hrtli_cpp/{benchmark_lits_comparison.cpp,test_competitor_set_adapters.cpp,packed_rank_transport.hpp,prefix_radix_delta.hpp,approx_local_delta.hpp} "$out/source_v2/"
cp scripts/verify_current_competitors_corrected.sh "$out/source_v2/"
includes=(-I "$out/source_v2" -I "$build/LITS" -I "$build/libart/src"
    -I "$build/hot/libs/hot/single-threaded/include"
    -I "$build/hot/libs/hot/commons/include" -I "$build/hot/libs/idx/content-helpers/include")
flags=(-O1 -g0 -march=native -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
g++ -std=c++17 "${flags[@]}" -DHRTLI_WITH_ART_HOT "${includes[@]}" \
    "$out/source_v2/test_competitor_set_adapters.cpp" "$build/art_sanitized.o" -o "$build/adapters_corrected"
for mode in hrtli art hot lits; do
    python3 scripts/record_experiment.py --output "$out/adapters_v2_$mode.json" \
        --source "$out/source_v2/benchmark_lits_comparison.cpp" --source "$out/source_v2/test_competitor_set_adapters.cpp" \
        --source "$out/source_v2/packed_rank_transport.hpp" --source "$out/source_v2/prefix_radix_delta.hpp" \
        --source scripts/patches/lits-zero-prefix-context.patch --source "$build/adapters_corrected" \
        -- "$build/adapters_corrected" "$build" "$mode"
done
head -n 5 /tmp/hrtli_lits_scale_yXM89Z/corpus/sample_10000000/base.txt
