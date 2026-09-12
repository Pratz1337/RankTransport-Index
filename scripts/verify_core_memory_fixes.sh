#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/sayal/OneDrive/Desktop/research
check_dir=$(mktemp -d /tmp/hrtli_core_memory_XXXXXX)
printf 'Temporary check directory: %s\n' "$check_dir"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-mismatched-new-delete \
    hrtli_cpp/test_prefix_radix_allocation.cpp -o "$check_dir/allocation_check"
"$check_dir/allocation_check"
g++ -std=c++17 -O2 -Wall -Wextra -Wno-mismatched-new-delete \
    hrtli_cpp/test_mapped_allocation.cpp -o "$check_dir/mapped_allocation_check"
"$check_dir/mapped_allocation_check"
g++ -std=c++17 -O2 -Wall -Wextra hrtli_cpp/test_prefix_radix_delta.cpp \
    -o "$check_dir/radix_check"
"$check_dir/radix_check"
printf 'PASS: existing 5000-operation radix oracle\n'
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    hrtli_cpp/test_packed_certification.cpp -o "$check_dir/packed_check"
ASAN_OPTIONS=detect_leaks=1 "$check_dir/packed_check"
# Same regression against the immutable pre-fix header must fail.
cp hrtli_cpp/test_prefix_radix_allocation.cpp "$check_dir/test_old.cpp"
g++ -std=c++17 -O2 -Wno-mismatched-new-delete \
    -I "$PWD/results_q1/reassessment_20260905/query_paths_source_v1" \
    "$check_dir/test_old.cpp" -o "$check_dir/old_allocation_check"
if "$check_dir/old_allocation_check"; then
    printf 'FAIL: regression unexpectedly passed against pre-fix ledger\n' >&2
    exit 1
fi
printf 'PASS: pre-fix ledger rejected by the same allocation-failure regression\n'
cp hrtli_cpp/test_mapped_allocation.cpp "$check_dir/test_old_mapped.cpp"
g++ -std=c++17 -O2 -Wno-mismatched-new-delete \
    -I "$PWD/results_q1/reassessment_20260905/query_paths_source_v1" \
    "$check_dir/test_old_mapped.cpp" -o "$check_dir/old_mapped_check"
if "$check_dir/old_mapped_check"; then
    printf 'FAIL: pre-fix mapped constructor unexpectedly passed resource checks\n' >&2
    exit 1
fi
printf 'PASS: pre-fix mapped constructor rejected by the same allocation-failure regression\n'
