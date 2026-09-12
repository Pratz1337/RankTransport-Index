#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/adaptive_competitors_20260906
mkdir "$out" "$out/source_measured" "$out/patches"
upstream_build=/tmp/hrtli_current_competitors_Pmjw3A
build=$(mktemp -d /tmp/hrtli_adaptive_competitors_XXXXXX)
printf '%s\n' "$build" > "$out/build_path.txt"
cp hrtli_cpp/{benchmark_lits_comparison.cpp,test_competitor_set_adapters.cpp,packed_rank_transport.hpp,prefix_radix_delta.hpp,approx_local_delta.hpp} "$out/source_measured/"
cp scripts/{build_adaptive_competitors.sh,record_experiment.py} "$out/source_measured/"
cp results_q1/current_competitors_20260906/{upstream_revisions.txt,compiler.txt,cpu.txt} "$out/"
cp results_q1/current_competitors_20260906/patches/*.patch "$out/patches/"
bash scripts/verify_radix_fenwick.sh > "$out/core_verification.txt" 2>&1
hot_includes=(-I "$out/source_measured" -I "$upstream_build/libart/src"
    -I "$upstream_build/hot/libs/hot/single-threaded/include"
    -I "$upstream_build/hot/libs/hot/commons/include" -I "$upstream_build/hot/libs/idx/content-helpers/include")
g++ -std=c++17 -O0 -g0 -march=native -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DHRTLI_WITH_ART_HOT "${hot_includes[@]}" "$out/source_measured/test_competitor_set_adapters.cpp" \
    "$upstream_build/art_sanitized.o" -o "$build/adapters"
python3 scripts/record_experiment.py --output "$out/adaptive_set_verification.json" \
    --source "$out/source_measured/benchmark_lits_comparison.cpp" --source "$out/source_measured/prefix_radix_delta.hpp" \
    --source "$out/source_measured/packed_rank_transport.hpp" --source "$build/adapters" \
    -- "$build/adapters" "$build" hrtli
g++ -std=c++17 -O3 -DNDEBUG -march=native -DHRTLI_WITH_ART_HOT "${hot_includes[@]}" \
    -MMD -MF "$out/measured_art_hot_dependencies.d" "$out/source_measured/benchmark_lits_comparison.cpp" \
    "$upstream_build/art_optimized.o" -o "$build/comparison_art_hot"
g++ -std=c++17 -O3 -DNDEBUG -march=native -I "$out/source_measured" -I "$upstream_build/LITS" \
    -MMD -MF "$out/measured_lits_dependencies.d" "$out/source_measured/benchmark_lits_comparison.cpp" \
    -o "$build/comparison_lits"
! grep -q '/LITS/' "$out/measured_art_hot_dependencies.d"
! grep -q '/hot/libs/' "$out/measured_lits_dependencies.d"
sha256sum "$build/comparison_art_hot" "$build/comparison_lits" > "$out/binaries.sha256"
