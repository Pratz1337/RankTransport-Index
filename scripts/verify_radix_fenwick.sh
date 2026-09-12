#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
check_dir=$(mktemp -d /tmp/hrtli_radix_fenwick_XXXXXX)
printf 'Build directory: %s\n' "$check_dir"
sha256sum hrtli_cpp/prefix_radix_delta.hpp hrtli_cpp/packed_rank_transport.hpp
for name in test_prefix_radix_delta test_prefix_radix_allocation test_prefix_radix_fanout test_packed_certification; do
  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Ihrtli_cpp "hrtli_cpp/$name.cpp" -o "$check_dir/$name"
  ASAN_OPTIONS=detect_leaks=1 "$check_dir/$name"
  printf 'PASS %s\n' "$name"
done
