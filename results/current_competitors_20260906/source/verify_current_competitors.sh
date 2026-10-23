#!/usr/bin/env bash
set -euo pipefail
workspace=/mnt/c/Users/sayal/OneDrive/Desktop/research
cd "$workspace"
out=results_q1/current_competitors_20260906
mkdir "$out"
build=$(mktemp -d /tmp/hrtli_current_competitors_XXXXXX)
printf 'Current competitor build: %s\n' "$build"
printf '%s\n' "$build" > "$out/build_path.txt"
mkdir "$build/libart" "$build/hot" "$out/source"
for entry in libart:301046804af165269e37da6725f5a4aec9ecc881 hot:96bf6fb7103b27e50e16a6026db8974c090ee84a lits:6f4793dff0dcf66daada49e2f4ae1b1838bfa9cc; do
    name=${entry%%:*}; revision=${entry#*:}
    upstream="$workspace/external/competitors/$name"
    test "$(git -C "$upstream" rev-parse HEAD)" = "$revision"
    test -z "$(git -C "$upstream" -c core.autocrlf=true -c core.filemode=false status --porcelain)"
    printf '%s %s\n' "$name" "$revision" >> "$out/upstream_revisions.txt"
    if test "$name" = lits; then
        git -C "$upstream" archive "$revision" LITS | tar -x -C "$build"
    else
        git -C "$upstream" archive "$revision" | tar -x -C "$build/$name"
    fi
done
for change in lits-pmss-bounds lits-hot-portability lits-iterator-root lits-hot-iterator-assignment lits-hot-empty-mask; do
    (cd "$build/LITS" && git apply "$workspace/scripts/patches/$change.patch")
done
cp hrtli_cpp/{benchmark_lits_comparison.cpp,test_competitor_set_adapters.cpp,packed_rank_transport.hpp,prefix_radix_delta.hpp,approx_local_delta.hpp} "$out/source/"
cp scripts/{verify_current_competitors.sh,record_experiment.py} "$out/source/"
includes=(-I "$out/source" -I "$build/LITS" -I "$build/libart/src"
    -I "$build/hot/libs/hot/single-threaded/include"
    -I "$build/hot/libs/hot/commons/include" -I "$build/hot/libs/idx/content-helpers/include")
flags=(-O1 -g0 -march=native -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
gcc -std=c11 "${flags[@]}" -c "$build/libart/src/art.c" -o "$build/art_sanitized.o"
g++ -std=c++17 "${flags[@]}" -DHRTLI_WITH_ART_HOT "${includes[@]}" \
    "$out/source/test_competitor_set_adapters.cpp" "$build/art_sanitized.o" -o "$build/adapters_sanitized"
python3 scripts/record_experiment.py --output "$out/adapters_upstream_hot.json" \
    --source "$out/source/benchmark_lits_comparison.cpp" --source "$out/source/test_competitor_set_adapters.cpp" \
    --source "$out/source/packed_rank_transport.hpp" --source "$out/source/prefix_radix_delta.hpp" \
    --source "$build/adapters_sanitized" -- "$build/adapters_sanitized" "$build"
