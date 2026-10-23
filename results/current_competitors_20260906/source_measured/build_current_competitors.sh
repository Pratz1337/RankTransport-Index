#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_competitors_20260906
build=$(< "$out/build_path.txt")
test "$build" = /tmp/hrtli_current_competitors_Pmjw3A
(cd "$build/hot" && git apply "$workspace/scripts/patches/hot-portable-mask.patch")
mkdir "$out/source_measured" "$out/patches"
cp hrtli_cpp/{benchmark_lits_comparison.cpp,test_competitor_set_adapters.cpp,packed_rank_transport.hpp,prefix_radix_delta.hpp,approx_local_delta.hpp} "$out/source_measured/"
cp scripts/{build_current_competitors.sh,record_experiment.py} "$out/source_measured/"
cp scripts/patches/lits-*.patch scripts/patches/hot-portable-mask.patch "$out/patches/"
g++ --version > "$out/compiler.txt"
lscpu > "$out/cpu.txt"
flags=(-O0 -g0 -march=native -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
hot_includes=(-I "$out/source_measured" -I "$build/libart/src"
    -I "$build/hot/libs/hot/single-threaded/include"
    -I "$build/hot/libs/hot/commons/include" -I "$build/hot/libs/idx/content-helpers/include")
g++ -std=c++17 "${flags[@]}" -DHRTLI_WITH_ART_HOT "${hot_includes[@]}" \
    -MMD -MF "$out/current_art_hot_dependencies.d" "$out/source_measured/test_competitor_set_adapters.cpp" \
    "$build/art_sanitized.o" -o "$build/adapters_portable_art_hot"
! grep -q '/LITS/' "$out/current_art_hot_dependencies.d"
sources=(--source "$out/source_measured/benchmark_lits_comparison.cpp"
    --source "$out/source_measured/test_competitor_set_adapters.cpp"
    --source "$out/source_measured/packed_rank_transport.hpp" --source "$out/source_measured/prefix_radix_delta.hpp"
    --source "$out/source_measured/approx_local_delta.hpp" --source "$out/source_measured/build_current_competitors.sh"
    --source "$out/upstream_revisions.txt")
for patch in "$out"/patches/*.patch; do sources+=(--source "$patch"); done
for mode in hrtli art hot; do
    python3 scripts/record_experiment.py --output "$out/adapters_portable_$mode.json" "${sources[@]}" \
        --source "$build/adapters_portable_art_hot" -- "$build/adapters_portable_art_hot" "$build" "$mode"
done
g++ -std=c++17 "${flags[@]}" -I "$out/source_measured" -I "$build/LITS" \
    -MMD -MF "$out/current_lits_dependencies.d" "$out/source_measured/test_competitor_set_adapters.cpp" \
    -o "$build/adapters_portable_lits"
! grep -q '/hot/libs/' "$out/current_lits_dependencies.d"
# Retained LSAN failure is not waived as a clean test: this second check isolates
# address/undefined behavior. Finite-trace RSS includes unrepaired LITS leaks.
python3 scripts/record_experiment.py --output "$out/adapters_portable_lits_address_ubsan.json" "${sources[@]}" \
    --source "$build/adapters_portable_lits" \
    -- env ASAN_OPTIONS=detect_leaks=0 "$build/adapters_portable_lits" "$build" lits
gcc -std=c11 -O3 -DNDEBUG -march=native -c "$build/libart/src/art.c" -o "$build/art_optimized.o"
g++ -std=c++17 -O3 -DNDEBUG -march=native -DHRTLI_WITH_ART_HOT "${hot_includes[@]}" \
    -MMD -MF "$out/measured_art_hot_dependencies.d" "$out/source_measured/benchmark_lits_comparison.cpp" \
    "$build/art_optimized.o" -o "$build/comparison_art_hot"
g++ -std=c++17 -O3 -DNDEBUG -march=native -I "$out/source_measured" -I "$build/LITS" \
    -MMD -MF "$out/measured_lits_dependencies.d" "$out/source_measured/benchmark_lits_comparison.cpp" \
    -o "$build/comparison_lits"
! grep -q '/LITS/' "$out/measured_art_hot_dependencies.d"
! grep -q '/hot/libs/' "$out/measured_lits_dependencies.d"
sha256sum "$build/comparison_art_hot" "$build/comparison_lits" > "$out/binaries.sha256"
printf 'Frozen current comparison binaries are ready.\n'
