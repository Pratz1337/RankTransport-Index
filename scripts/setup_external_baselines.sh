#!/usr/bin/env bash
# Clone and pin optional external baselines used by hrtli_cpp/benchmark_external.cpp.

set -euo pipefail

ROOT="${EXTERNAL_ROOT:-external/competitors}"
mkdir -p "$ROOT"

pin_repo() {
  local name="$1"
  local url="$2"
  local commit="$3"
  local dir="$ROOT/$name"

  if [ ! -d "$dir/.git" ]; then
    git clone "$url" "$dir"
  fi
  git -C "$dir" fetch --quiet origin "$commit"
  git -C "$dir" checkout --quiet "$commit"
  printf '%-8s %s %s\n' "$name" "$commit" "$url"
}

pin_repo "alex" "https://github.com/microsoft/ALEX.git" "4370da6aa8b509fdc9b0d2c49faa0624b0078589"
pin_repo "pgm" "https://github.com/gvinciguerra/PGM-index.git" "c6fcf3d34e55eb0061b01e2f49dfcbdb711f1407"
pin_repo "lipp" "https://github.com/Jiacheng-WU/LIPP.git" "fe6ca4954f00875482f9e4dd63b34dae2384d23b"
pin_repo "libart" "https://github.com/armon/libart.git" "301046804af165269e37da6725f5a4aec9ecc881"
pin_repo "hot" "https://github.com/speedskater/hot.git" "96bf6fb7103b27e50e16a6026db8974c090ee84a"
if [ "${INCLUDE_LITS:-0}" = "1" ]; then
  pin_repo "lits" "https://github.com/schencoding/lits.git" "c9026ac9645b4af1f6e3cb27b42351220f4376d4"
else
  echo "Skipping LITS by default: no root LICENSE file was found at the pinned revision."
fi

echo "Pinned external baselines under $ROOT"
