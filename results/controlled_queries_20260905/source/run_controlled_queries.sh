#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/sayal/OneDrive/Desktop/research
first_trial=${1:-1}
last_trial=${2:-1}
output="$PWD/results_q1/controlled_queries_20260905"
source_dir="$output/source"
binary=/tmp/hrtli_controlled_queries_20260905
if [[ ! -d "$output" ]]; then
    mkdir -p "$source_dir"
    cp hrtli_cpp/*.hpp hrtli_cpp/benchmark_packed_query_paths.cpp "$source_dir/"
    cp scripts/record_experiment.py scripts/run_controlled_queries.sh "$source_dir/"
    (cd "$source_dir" && sha256sum ./* > ../source.sha256)
    g++ -std=c++17 -O3 -DNDEBUG -march=native "$source_dir/benchmark_packed_query_paths.cpp" -o "$binary"
    g++ --version > "$output/compiler.txt"
    uname -a > "$output/kernel.txt"
    lscpu > "$output/cpu.txt"
    systemctl --no-pager --type=service --state=running > "$output/services.txt"
    sha256sum "$binary" > "$output/binary.sha256"
    # Small natural-data regression before any full-scale timing.
    "$binary" /tmp/hrtli_real_smoke_20260905/initial.txt /tmp/hrtli_real_smoke_20260905/insert.txt \
        199900 2000 64 20000 4 learned_binary > "$output/smoke.json"
fi
(cd "$source_dir" && sha256sum -c ../source.sha256 >/dev/null)
sha256sum -c "$output/binary.sha256" >/dev/null
base="$PWD/tmp/commoncrawl-2026-may-jun-jul-local/prepared_interleaved/commoncrawl_hosts_200m_initial.txt"
insert="$PWD/tmp/commoncrawl-2026-may-jun-jul-local/prepared_interleaved/commoncrawl_hosts_200m_insert.txt"
for trial in $(seq "$first_trial" "$last_trial"); do
    record="$output/trial_${trial}.json"
    if [[ -e "$record" || -e "$output/trial_${trial}.vmstat.txt" ]]; then
        printf 'Refusing to overwrite trial %s\n' "$trial" >&2
        exit 2
    fi
    printf 'START trial %s UTC %s\n' "$trial" "$(date -u +%FT%TZ)"
    awk '$1=="pswpin" || $1=="pswpout" {print}' /proc/vmstat > "$output/trial_${trial}.swap_before.txt"
    vmstat -w 5 > "$output/trial_${trial}.vmstat.txt" &
    monitor_pid=$!
    trap 'kill "$monitor_pid" 2>/dev/null || true' EXIT
    python3 "$source_dir/record_experiment.py" --output "$record" \
        --source "$source_dir/packed_rank_transport.hpp" \
        --source "$source_dir/prefix_radix_delta.hpp" \
        --source "$source_dir/benchmark_packed_query_paths.cpp" --source "$binary" \
        -- /usr/bin/time -v taskset -c 2 "$binary" "$base" "$insert" \
        199900000 2000 64 1000000 4 learned_binary > "$output/trial_${trial}.runner.json"
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
    trap - EXIT
    awk '$1=="pswpin" || $1=="pswpout" {print}' /proc/vmstat > "$output/trial_${trial}.swap_after.txt"
    python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); x=r["result"]; assert r["exit_code"]==0 and x["all_oracles_passed"] and x["fingerprint_capacity"]==0 and len(x["rows"])==8; print("PASS correctness gate:",sys.argv[1],flush=True)' "$record"
    if ! cmp -s "$output/trial_${trial}.swap_before.txt" "$output/trial_${trial}.swap_after.txt"; then
        printf 'STOP: guest swap activity invalidates controlled timing; preserve this trial for diagnosis\n' >&2
        exit 3
    fi
    printf 'COMPLETE trial %s UTC %s\n' "$trial" "$(date -u +%FT%TZ)"
done
