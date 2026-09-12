#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/sayal/OneDrive/Desktop/research
check_dir=$(mktemp -d /tmp/hrtli_default_queries_XXXXXX)
printf 'Temporary check directory: %s\n' "$check_dir"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    hrtli_cpp/test_packed_certification.cpp -o "$check_dir/certification"
ASAN_OPTIONS=detect_leaks=1 "$check_dir/certification"
g++ -std=c++17 -O2 hrtli_cpp/test_packed_rank_transport.cpp -o "$check_dir/annihilation"
"$check_dir/annihilation"
for source in benchmark_packed_query_paths benchmark_commoncrawl_200m benchmark_commoncrawl_200m_lifecycle; do
    g++ -std=c++17 -O2 "hrtli_cpp/$source.cpp" -o "$check_dir/$source"
done
printf 'PASS: dependent drivers compile with explicit legacy/control query paths\n'
printf 'Natural 200000-key API regression only; these timings are not manuscript evidence.\n'
"$check_dir/benchmark_packed_query_paths" \
    /tmp/hrtli_real_smoke_20260905/initial.txt /tmp/hrtli_real_smoke_20260905/insert.txt \
    199900 2000 64 20000 3
