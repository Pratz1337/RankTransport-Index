#!/usr/bin/env bash
set -euo pipefail
cd /mnt/c/Users/sayal/OneDrive/Desktop/research
check_dir=$(mktemp -d /tmp/hrtli_segment_fence_XXXXXX)
printf 'Temporary check directory: %s\n' "$check_dir"
# A mechanically transformed temporary header tests the fallback independently
# of normal model quality. The production header is never modified here.
sed 's/const int64_t predicted = predict_segment(segment, packed_key_to_double(key));/const int64_t predicted = std::numeric_limits<int>::max();/' \
    hrtli_cpp/packed_rank_transport.hpp > "$check_dir/packed_rank_transport.hpp"
cp hrtli_cpp/test_packed_certification.cpp "$check_dir/test_packed_certification.cpp"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I hrtli_cpp "$check_dir/test_packed_certification.cpp" -o "$check_dir/injected"
ASAN_OPTIONS=detect_leaks=1 "$check_dir/injected" --injected-prediction
printf 'PASS injected out-of-window predictions recover through exact segment fences\n'
