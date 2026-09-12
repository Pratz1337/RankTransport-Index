#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_competitors_20260906
build=$(< "$out/build_path.txt")
test "$build" = /tmp/hrtli_current_competitors_Pmjw3A
corpus=/tmp/hrtli_lits_scale_yXM89Z/corpus/sample_10000000
sha256sum --check "$out/binaries.sha256"
cp scripts/{run_current_competitors.sh,run_current_competitors.ps1,check_current_comparison_inputs.py} "$out/source_measured/"
python3 scripts/check_current_comparison_inputs.py "$corpus" "$out"
sources=(--source "$out/PROTOCOL.md" --source "$out/verified_inputs.json" --source "$out/upstream_revisions.txt"
    --source "$build/comparison_art_hot" --source "$build/comparison_lits"
    --source "$out/source_measured/run_current_competitors.sh" --source "$out/source_measured/run_current_competitors.ps1")
for name in benchmark_lits_comparison.cpp packed_rank_transport.hpp prefix_radix_delta.hpp approx_local_delta.hpp; do
    sources+=(--source "$out/source_measured/$name")
done
for file in "$out"/patches/*.patch; do sources+=(--source "$file"); done
modes=(hrtli art hot lits)
for cell in readonly uniform50 prefix50; do
    writes=50; family=valid_writes
    if test "$cell" = readonly; then writes=0; fi
    if test "$cell" = prefix50; then family=prefix_bursts; fi
    for trial in 0 1 2 3; do
        seed=$((20260906 + trial))
        for order in 0 1 2 3; do
            mode=${modes[$(((trial + order) % 4))]}
            binary="$build/comparison_art_hot"
            if test "$mode" = lits; then binary="$build/comparison_lits"; fi
            name="${cell}_${seed}_${mode}"
            awk '/pswpin |pswpout /' /proc/vmstat > "$out/${name}_swap_before.txt"
            python3 scripts/record_experiment.py --output "$out/${name}.json" "${sources[@]}" \
                -- taskset -c 2 "$binary" "$mode" "$corpus/base.txt" "$corpus/base.nul" "$corpus/arrivals.nul" \
                8000000 2000000 1048576 "$writes" "$seed" 64 "$family"
            awk '/pswpin |pswpout /' /proc/vmstat > "$out/${name}_swap_after.txt"
        done
    done
done
