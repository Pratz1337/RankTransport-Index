#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/sayal/OneDrive/Desktop/research
out=results_q1/learned_mutation_20260905
mkdir "$out"
pilot=/tmp/hrtli_lits_scale_yXM89Z
test -x "$pilot/comparison"
test -f "$pilot/corpus/sample_10000000/provenance.json"
build=$(mktemp -d /tmp/hrtli_learned_mutation_XXXXXX)
# Identical driver and all other headers; only the two mutation base probes differ.
cmp hrtli_cpp/benchmark_lits_comparison.cpp results_q1/lits_scale_v2_20260905/source/benchmark_lits_comparison.cpp
mkdir "$out/source"
cp hrtli_cpp/packed_rank_transport.hpp hrtli_cpp/benchmark_lits_comparison.cpp \
    hrtli_cpp/prefix_radix_delta.hpp scripts/run_learned_mutation_ablation.sh "$out/source/"
g++ -std=c++17 -O3 -DNDEBUG -march=native -I "$pilot/LITS" \
    hrtli_cpp/benchmark_lits_comparison.cpp -o "$build/learned_mutation"
g++ --version > "$out/compiler.txt"
uname -a > "$out/kernel.txt"
lscpu > "$out/cpu.txt"
corpus="$pilot/corpus/sample_10000000"
for trial in 1 2 3; do
    modes=(binary_mutation learned_mutation)
    if (( trial % 2 == 0 )); then modes=(learned_mutation binary_mutation); fi
    for variant in "${modes[@]}"; do
        binary="$pilot/comparison"
        header=results_q1/lits_scale_v2_20260905/source/packed_rank_transport.hpp
        if [[ "$variant" = learned_mutation ]]; then
            binary="$build/learned_mutation"
            header=hrtli_cpp/packed_rank_transport.hpp
        fi
        name="${trial}_${variant}"
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${name}_swap_before.txt"
        python3 scripts/record_experiment.py --output "$out/$name.json" \
            --source "$binary" --source "$header" --source scripts/run_learned_mutation_ablation.sh \
            --source hrtli_cpp/benchmark_lits_comparison.cpp --source hrtli_cpp/prefix_radix_delta.hpp \
            --source "$corpus/provenance.json" --source "$corpus/base.txt" \
            --source "$corpus/base.nul" --source "$corpus/arrivals.nul" \
            -- taskset -c 2 "$binary" hrtli "$corpus/base.txt" "$corpus/base.nul" \
            "$corpus/arrivals.nul" 8000000 2000000 1000000 50 20260905 64 valid_writes
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${name}_swap_after.txt"
    done
done
