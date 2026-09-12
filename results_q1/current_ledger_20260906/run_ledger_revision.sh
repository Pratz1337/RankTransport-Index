#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_ledger_20260906
mkdir "$out" "$out/linear" "$out/fenwick"
cp hrtli_cpp/benchmark_ledger_revision.cpp "$out/linear/"
cp hrtli_cpp/benchmark_ledger_revision.cpp "$out/fenwick/"
cp results_q1/reviewer_response_20260906/before/prefix_radix_delta.hpp "$out/linear/"
cp results_q1/current_competitors_20260906/source_measured/prefix_radix_delta.hpp "$out/fenwick/"
cp scripts/run_ledger_revision.sh "$out/"
build=$(mktemp -d /tmp/hrtli_ledger_revision_XXXXXX)
corpus=/tmp/hrtli_lits_scale_yXM89Z/corpus/sample_10000000
for variant in linear fenwick; do
    g++ -std=c++17 -O3 -DNDEBUG -march=native "$out/$variant/benchmark_ledger_revision.cpp" -o "$build/$variant"
done
for seed in 20260906 20260907 20260908 20260909; do
    variants=(linear fenwick)
    if ((seed % 2)); then variants=(fenwick linear); fi
    for variant in "${variants[@]}"; do
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${seed}_${variant}_swap_before.txt"
        python3 scripts/record_experiment.py --output "$out/${seed}_${variant}.json" \
            --source "$out/$variant/benchmark_ledger_revision.cpp" --source "$out/$variant/prefix_radix_delta.hpp" \
            --source scripts/run_ledger_revision.sh --source "$build/$variant" \
            --source results_q1/current_competitors_20260906/verified_inputs.json \
            --source "$corpus/base.txt" --source "$corpus/arrivals.nul" \
            -- taskset -c 2 "$build/$variant" "$corpus/base.txt" "$corpus/arrivals.nul" "$seed" "$variant"
        awk '/pswpin |pswpout /' /proc/vmstat > "$out/${seed}_${variant}_swap_after.txt"
    done
done
