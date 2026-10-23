# Reproducibility: result files and what produced them

Every file under `results/` is a frozen experiment record. `make verify`
(`scripts/verify_release.py`) checks the SHA-256 of all 136 indexed records against
[`docs/BUILD_PROVENANCE.json`](docs/BUILD_PROVENANCE.json), which also lists, per record, the
experiment-time source and binary digests where they were recorded.

Conventions used below:

* **Producer** is the program that wrote the file; **runner** is the script that invoked it.
* Timed native runs were wrapped by `scripts/record_experiment.py`, which stores the exact
  command, platform, source digests, exit code, stdout/stderr and resource usage in the JSON.
* Records are kept byte-for-byte as produced. Paths *inside* records (commands, source
  directories) therefore still show the machine and directory names used at run time, e.g.
  `/tmp/hrtli_lits_scale_*/corpus/sample_10000000/…` or the earlier `results_q1/` directory name,
  which is now `results/`. The 200M commands read
  `tmp/commoncrawl-2026-may-jun-jul-local/prepared_interleaved/`, the output directory of
  `scripts/prepare_commoncrawl_200m.py` used at the time (see
  [`docs/DATA_AND_200M_REPRODUCTION.md`](docs/DATA_AND_200M_REPRODUCTION.md)).
* Several records store the SHA-256 of the runner script that produced them. Those runners have
  since been edited only to resolve paths relative to the repository; the exact versions that
  ran are kept unmodified in [`scripts/as_run/`](scripts/as_run) so that the recorded digests
  still verify.
* Where an experiment froze its own copy of the sources (`source/`, `source_measured/`,
  `*_source_v1/`, `runtime_source/`), that copy, not the current `hrtli_cpp/`, is what was
  compiled.

## Paper figures and table

| Paper item | Script | Inputs |
|---|---|---|
| Fig. 1 | `figures/fig1_overview.tex` (`make fig1`) | none |
| Fig. 2 | `figures/make_figs.py` → `fig_scale.pdf` | `verification_20260905/epsilon_corrected.json`, `controlled_queries_20260905/analysis.json`, `verification_20260905/signed_consolidation_200m.json` |
| Fig. 3 | `figures/make_figs.py` → `fig_specialists.pdf` | `adaptive_competitors_20260906/summary.json` |
| Fig. 4 | `figures/make_figs.py` → `fig_rankcap.pdf` | `replicated_rankcap/replicated_summary.json` |
| Table 1 | `figures/make_table1.py` | `replicated_rankcap/replicated_summary.json` |
| Fig. 5 | `figures/make_figs.py` → `fig_range.pdf` | `benchmark_audit/range_stable_commoncrawl_host_real.json`, `replicated_rankcap/replicated_summary.json` |
| Fig. 6 | `figures/make_figs.py` → `fig_ledger.pdf` | `adaptive_ledger_20260906/*_{linear,fenwick,adaptive}.json`, `early_prototype/prefix_radix_delta.json`, `early_prototype/cpp_benchmark_results.json` |

Each plotted value is read from these files; none is entered by hand.

## Results by directory

### `results/replicated_rankcap/` — rank-capable comparison (Fig. 4, Fig. 5b–c, Table 1)

| File | Producer |
|---|---|
| `repl{1..5}_cc_hosts_10m_d80000_s20261023_e64.json`, `repl{1..5}_enwiki_titles_d150000_s20261023_e64.json` | `scripts/modal/modal_hrtli.py` (`equal_contract_session`), one fresh Modal container per file; launched by `scripts/modal/modal_replicate.py`; driver `hrtli_cpp/benchmark_equal_contract.cpp` (with `counted_art.hpp`, `fenced_rank_transport.hpp`, `counted_radix_ost.hpp`, `packed_rank_transport.hpp`, `prefix_radix_delta.hpp`, `approx_local_delta.hpp`, all byte-identical to the recorded digests), built with g++ 12.2.0 and `-std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -DHRTLI_WITH_COUNTED_ART -DHRTLI_WITH_FENCED`; delta 80,000 (hosts) / 150,000 (titles), 1,048,576 queries, seed 20261023, ε = 64. Each record stores its build command, source digests, `/proc/cpuinfo` summary and telemetry. |
| `replicated_summary.json` | `figures/summarize_replicates.py` over the ten replicate records (`make replicated-summary`). |
| `corpus_commoncrawl.json` | `scripts/modal/modal_hrtli.py::run_prepare_commoncrawl` (shard downloads, regenerated 200M split, 1M/10M sample provenance). |
| `corpus_enwiki_titles.json` | `scripts/modal/modal_hrtli.py::run_prepare_enwiki`. |
| `corpus_ubuntu_noble_paths.json` | `scripts/modal/modal_hrtli.py::run_prepare_paths` (prepared, not used in the paper). |

See [`scripts/modal/README.md`](scripts/modal/README.md) for the source digests of the
benchmark driver and headers used in these runs. `make equal-contract-smoke` builds the driver
and checks every method on a small synthetic key set (correctness only).

Earlier experiments compiled older revisions of `packed_rank_transport.hpp` and
`counted_radix_ost.hpp`; each such experiment kept its own frozen copy (for example
`adaptive_competitors_20260906/source_measured/`), so the newer files in `hrtli_cpp/` do not
affect those records.

### `results/verification_20260905/` — 200M scale evidence and correctness verification

| File | Producer |
|---|---|
| `epsilon_corrected.json` | `epsilon_source_v1/benchmark_commoncrawl_200m_epsilon.cpp` (frozen sources alongside), via `record_experiment.py`, pinned with `taskset -c 2`, on the 199.9M base. |
| `legacy_epsilon_reproduction.json` | Earlier build of `hrtli_cpp/benchmark_commoncrawl_200m_epsilon.cpp` on the same input. |
| `signed_consolidation_200m.json` | `signed_source_v1/benchmark_signed_consolidation.cpp` (frozen sources and `record_experiment.py` alongside). |
| `controlled_{1_to_1,2_to_2,3_to_6}.{stdout,stderr}.txt`, `*.host.csv` | Host-side logs of `scripts/run_controlled_queries.ps1` for the controlled-query trials (records in `controlled_queries_20260905/`). |
| `core_memory_fix_final_verification.json` | `scripts/verify_core_memory_fixes.sh` (as run: `scripts/as_run/`). |
| `default_query_api_verification.json`, `learned_mutation_verification.txt` | `scripts/verify_default_query_paths.sh` (as run: `scripts/as_run/`). |
| `segment_fence_verification.txt` | `scripts/verify_segment_fence.sh`. |
| `automatic_maintenance_before_after_final.json` | `scripts/verify_automatic_maintenance.py` (as run: `scripts/as_run/`), comparing against `automatic_maintenance_source_before/`. |
| `python_transaction_before_after_final.json` | `scripts/verify_python_write_transactions.py` (as run: `scripts/as_run/`), comparing against `python_transaction_source_before/`. |
| `automatic_maintenance_and_seed_regression_final.json`, `automatic_maintenance_regression_final.json`, `python_transaction_regression_final.json`, `compact_oracle_regression.json`, `consolidation_failure_recovery.json`, `consolidation_hpsfc_regression.json`, `consolidation_rank_regression.json`, `consolidation_transaction_checks.json`, `dataset_seed_before.json`, `signed_delta_transaction_checks.json` | `python -m unittest` runs over `tests/` (the exact command is stored in each record). |
| `compact_evidence_audit.ipynb` | Audit notebook; checked by `scripts/audit_compact_evidence.py`. |

### `results/controlled_queries_20260905/` — learned vs. binary recovery (Fig. 2c)

| File | Producer |
|---|---|
| `trial_{2..6}.json`, `trial_*.runner.json`, `trial_*.swap_{before,after}.txt`, `trial_*.vmstat.txt`, `smoke.json` | `scripts/run_controlled_queries.sh` (as run: `scripts/as_run/`), driver `source/benchmark_packed_query_paths.cpp`. Trial 1 failed before launch (`/usr/bin/time` missing), so it has runner logs but no result JSON; `analysis.json` records this. |
| `analysis.json` | `scripts/analyze_controlled_queries.py` over the trial records and host logs. |
| `controlled_query_audit.ipynb` | Audit notebook over the same records. |
| `compiler.txt`, `cpu.txt`, `kernel.txt`, `services.txt`, `binary.sha256`, `source.sha256`, `source/`, `runtime_source/` | Build and host snapshot written by the runner. |

### `results/query_summary_20260906/`

| File | Producer |
|---|---|
| `query_descriptive.json` | `scripts/summarize_query_runs_descriptively.py`. |
| `fenwick_verification.txt` | `scripts/verify_radix_fenwick.sh`. |

### `results/adaptive_competitors_20260906/` — set-only workloads vs. ART, HOT, LITS (Fig. 3)

| File | Producer |
|---|---|
| `PROTOCOL.md` | Protocol fixed before timing. |
| `{readonly,uniform50,prefix50}_{seed}_{hrtli,art,hot,lits}.json` and `_swap_{before,after}.txt` (HRT-LI, ART, HOT, LITS; four seeds) | `scripts/run_current_competitors.sh results/adaptive_competitors_20260906` (as run: `scripts/as_run/`), launched from Windows by `scripts/run_current_competitors.ps1`; driver `source_measured/benchmark_lits_comparison.cpp`, binaries built by `scripts/build_adaptive_competitors.sh`. |
| `ledger_session.{stdout,stderr}.txt`, `ledger_session.host.csv` | Host-side logs of `scripts/run_ledger_revision.ps1 -Adaptive`, the Windows wrapper that launched `scripts/run_adaptive_ledger.sh` (records in `adaptive_ledger_20260906/`). |
| `summary.json` | `scripts/audit_current_comparisons.py --adaptive`. |
| `adaptive_set_verification.json`, `core_verification.txt` | Sanitizer/adapter checks in `scripts/build_adaptive_competitors.sh`. |
| `binaries.sha256`, `build_path.txt`, `compiler.txt`, `cpu.txt`, `*_dependencies.d`, `patches/` | Build snapshot (third-party revisions and patches). |
| `audit.ipynb` | Audit notebook. |

### `results/current_competitors_20260906/` — preceding specialist comparison

Same layout and runners as above, built by `scripts/build_current_competitors.sh`
(as run: `scripts/as_run/`); `adapters_*.json` are adapter checks from
`hrtli_cpp/test_competitor_set_adapters.cpp`. Superseded by `adaptive_competitors_20260906/`
and retained for completeness.

### `results/adaptive_ledger_20260906/` — ledger variants (Fig. 6a)

`{seed}_{linear,fenwick,adaptive}.json` and swap snapshots: `scripts/run_adaptive_ledger.sh`
(as run: `scripts/as_run/`), driver `benchmark_ledger_revision.cpp` (frozen copies under
`linear/`, `fenwick/`, `adaptive/`).

### `results/current_ledger_20260906/` — preceding ledger comparison

`{seed}_{linear,fenwick}.json`: `run_ledger_revision.sh` (kept in the directory) and
`scripts/run_ledger_revision.ps1`; frozen sources under `linear/`, `fenwick/`.

### `results/learned_mutation_20260905/`

`{1,2,3}_{binary,learned}_mutation.json`: `source/run_learned_mutation_ablation.sh` with the
frozen `source/benchmark_lits_comparison.cpp`; `audit.ipynb` audits them.

### `results/lits_scale_v2_20260905/`

`pilot_{1000000,10000000}_{hrtli,lits}.json`: pilot runs of `hrtli_cpp/benchmark_lits_comparison.cpp`
on the 1M and 10M samples (not reported in the paper).

### `results/benchmark_audit/` — earlier native index

| File | Producer |
|---|---|
| `range_stable_commoncrawl_host_real.json` (Fig. 5a) | `hrtli_cpp/benchmark_range_stable.cpp` on 9.04M host keys. The record carries no build identity (see `docs/BUILD_PROVENANCE.md`). |
| `external_benchmark_commoncrawl_host_real_art_hot.json` | `hrtli_cpp/benchmark_external.cpp` with ART/HOT; historical context only. |

### `results/early_prototype/` — earlier implementation (Fig. 6b–d) and exploratory runs

| File(s) | Producer |
|---|---|
| `prefix_radix_delta.json` (Fig. 6b) | `hrtli_cpp/benchmark_prefix_radix_delta.cpp` |
| `cpp_benchmark_results.json` (Fig. 6c–d) | `hrtli_cpp/benchmark.cpp` (`make cpp-benchmark`) |
| `cpp_avg_lookup_latency_ns.png`, `cpp_insert_time_ms.png`, `cpp_load_time_ms.png`, `cpp_lookup_throughput_mops.png` | `legacy/plot_cpp_results.py` |
| `ablation_hpsfc_results.json` | `hrtli_cpp/benchmark_ablation_hpsfc.cpp` (`make cpp-ablation-hpsfc`) |
| `ablation_results.json` | `benchmarks/python/benchmark_ablation.py` |
| `concurrent_timeline.json`, `concurrent_timeline_v2.json` | `hrtli_cpp/benchmark_concurrency.cpp` (`make cpp-concurrency-benchmark`) |
| `latency_vs_mn.json`, `memory_breakdown.json`, `model_evolution.json` | `hrtli_cpp/benchmark_characterization.cpp` (`make cpp-characterization-benchmark`) |
| `concurrent_timeline.png`, `latency_vs_mn.png`, `memory_breakdown.png`, `model_evolution.png` | `legacy/plot_characterization.py` |
| `cpp_range_latency_{osm_cellids,synthetic,wiki_ts}.json` | `hrtli_cpp/benchmark_range.cpp` (`make cpp-range-benchmark`) |
| `external_benchmark_url.json` | `hrtli_cpp/benchmark_external.cpp` (`make cpp-external`) |
| `hrtli_benchmark_results.json`, `*_rank_error.png`, `update_cost_comparison.png` | `benchmarks/python/benchmark_hrtli.py` (`make benchmark`) |
| `hpsfc_benchmark_results.json`, `hpsfc_vs_binary_search.png` | `benchmarks/python/benchmark_hpsfc.py` |
| `latency_comparison.png` | `legacy/plot_latency_v2_v3.py` |
| `range_degradation_results.{csv,json}` | `hrtli_cpp/benchmark_range_degradation.cpp` (`make cpp-range-degradation`) |
| `range_degradation_v2_results.{csv,json}` | `hrtli_cpp/benchmark_range_degradation_v2.cpp` |
| `range_latency_results.json` | `benchmarks/python/benchmark_range_latency.py` |
| `trie_depth_measurement.json` | `benchmarks/python/measure_trie_depth.py` |
| `write_sweep_results.{csv,json}` | `hrtli_cpp/benchmark_write_sweep.cpp` (`make cpp-write-sweep`) |
| `dynamic/` | `legacy/main_dynamic.py` (exploratory, not used in the paper) |

## Rebuilding from scratch

1. Corpora: [`docs/DATA_AND_200M_REPRODUCTION.md`](docs/DATA_AND_200M_REPRODUCTION.md)
   (200M split and 10M sample) and `scripts/modal/modal_hrtli.py` (`run_prepare_commoncrawl`,
   `run_prepare_enwiki`), which assert every published digest.
2. Specialist baselines: [`docs/CURRENT_COMPARISON_REPRODUCTION.md`](docs/CURRENT_COMPARISON_REPRODUCTION.md)
   and `scripts/setup_external_baselines.sh` (pinned ART/HOT/LITS revisions, `scripts/patches/`).
3. Timed runs: the runners listed above. Timings depend on the host; compare against the
   recorded CPU, compiler and telemetry, not only against the plotted numbers.
