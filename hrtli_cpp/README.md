# HRT-LI Native Benchmarks

## Portable Smoke Path

The portable benchmark needs only a C++17 compiler and the checked-in HRT-LI
headers:

```bash
make cpp-test
make cpp-benchmark
make cpp-range-benchmark
make cpp-external-smoke
```

`cpp-external-smoke` compiles `benchmark_external.cpp` with no third-party
baseline macros. It exists so CI can keep the external harness buildable without
vendoring competitor repositories.

`cpp-range-benchmark` compiles `benchmark_range.cpp` and writes
`results_q1/cpp_range_latency_synthetic.json`. The executable verifies
`count_range == scan_range.size()` for every measured range before reporting
latency percentiles.

## External Competitor Harness

The real external benchmark is opt-in. First clone the pinned checkouts:

```bash
EXTERNAL_ROOT=external/competitors bash scripts/setup_external_baselines.sh
```

Then run repeated trials:

```bash
make cpp-external EXTERNAL_DATASET=url EXTERNAL_TRIALS=5
```

`benchmark_external.cpp` prints mean and 95% confidence interval for load time,
insert time, lookup throughput, and lookup latency. It only benchmarks baselines
whose `HRTLI_WITH_*` compile macros are enabled. The default `cpp-external`
target enables ALEX, LIPP, PGM, ART, and HOT. LITS stays optional because the
pinned repository has no root license file; enable it explicitly only for local
source-only experiments after confirming redistribution constraints.

The numeric-surrogate rows for ALEX, LIPP, and PGM map string keys to stable
rank surrogates. They are an integration scaffold, not a final fairness claim
for string-key learned indexes.

## Pinned Baselines

The lockfile is `hrtli_cpp/BASELINES.lock.md`.

| Baseline | Macro | Repository | Commit | License note |
|---|---|---|---|---|
| ALEX | `HRTLI_WITH_ALEX` | https://github.com/microsoft/ALEX.git | `4370da6aa8b509fdc9b0d2c49faa0624b0078589` | MIT |
| PGM-index | `HRTLI_WITH_PGM` | https://github.com/gvinciguerra/PGM-index.git | `c6fcf3d34e55eb0061b01e2f49dfcbdb711f1407` | Apache-2.0 |
| LIPP | `HRTLI_WITH_LIPP` | https://github.com/Jiacheng-WU/LIPP.git | `fe6ca4954f00875482f9e4dd63b34dae2384d23b` | MIT |
| ART/libart | `HRTLI_WITH_ART` | https://github.com/armon/libart.git | `301046804af165269e37da6725f5a4aec9ecc881` | BSD-3-Clause-style |
| HOT | `HRTLI_WITH_HOT` | https://github.com/speedskater/hot.git | `96bf6fb7103b27e50e16a6026db8974c090ee84a` | ISC |
| LITS | `HRTLI_WITH_LITS` | https://github.com/schencoding/lits.git | `c9026ac9645b4af1f6e3cb27b42351220f4376d4` | No root LICENSE found; source-only optional baseline until permission is confirmed |
