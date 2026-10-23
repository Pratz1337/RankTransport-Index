#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/adaptive_ledger_20260906
mkdir "$out" "$out/linear" "$out/fenwick" "$out/adaptive"
for variant in linear fenwick adaptive; do cp hrtli_cpp/benchmark_ledger_revision.cpp "$out/$variant/"; done
cp results_q1/current_ledger_20260906/linear/prefix_radix_delta.hpp "$out/linear/"
cp results_q1/current_ledger_20260906/fenwick/prefix_radix_delta.hpp "$out/fenwick/"
cp results_q1/adaptive_competitors_20260906/source_measured/prefix_radix_delta.hpp "$out/adaptive/"
cp scripts/run_adaptive_ledger.sh "$out/"
build=$(mktemp -d /tmp/hrtli_adaptive_ledger_XXXXXX)
corpus=/tmp/hrtli_lits_scale_yXM89Z/corpus/sample_10000000
for variant in linear fenwick adaptive; do
    g++ -std=c++17 -O3 -DNDEBUG -march=native "$out/$variant/benchmark_ledger_revision.cpp" -o "$build/$variant"
done
variants=(linear fenwick adaptive)
for trial in 0 1 2 3 4 5; do
    seed=$((20260906 + trial))
    for order in 0 1 2; do
        variant=${variants[$(((trial + order) % 3))]}
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${seed}_${variant}_swap_before.txt"
        python3 scripts/record_experiment.py --output "$out/${seed}_${variant}.json" \
            --source "$out/$variant/benchmark_ledger_revision.cpp" --source "$out/$variant/prefix_radix_delta.hpp" \
            --source "$out/run_adaptive_ledger.sh" --source "$build/$variant" \
            --source results_q1/current_competitors_20260906/verified_inputs.json \
            --source "$corpus/base.txt" --source "$corpus/arrivals.nul" \
            -- taskset -c 2 "$build/$variant" "$corpus/base.txt" "$corpus/arrivals.nul" "$seed" "$variant"
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${seed}_${variant}_swap_after.txt"
    done
done
