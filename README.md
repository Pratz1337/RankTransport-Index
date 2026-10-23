# HRT-LI: Count, Don't Retrain

Code, frozen experiment records and figure scripts for

> **Count, Don't Retrain: Keeping a Frozen Learned String Index Certified Under Inserts and Deletes**
> Kshiraja Nelapati and Prathmesh Sayal

Learned indexes promise a bounded search window, but one write shifts every larger key and
silently voids that promise. For byte-string keys, rank drift is a bookkeeping problem, not a
learning problem. HRT-LI freezes a piecewise-linear model certified on a base snapshot `S_0` and
records the live set's signed difference from that snapshot in a byte-radix ledger. The ledger's
mass below a query is the exact drift, `rank_t(k) = rank_0(k) + Δ_t(k)`, so adding it to the
prediction keeps the certified error of every surviving base key unchanged after any write
sequence; new keys and absent boundaries are answered by exact counting instead. Range counts
need only two boundary recoveries. Retraining becomes a cost decision, never a correctness
requirement. The statements and proofs are summarised in [`docs/THEORY.md`](docs/THEORY.md).

## Repository layout

| Path | Contents |
|---|---|
| [`hrtli_cpp/`](hrtli_cpp) | C++17 core (`packed_rank_transport.hpp`, `prefix_radix_delta.hpp`, `approx_local_delta.hpp`, `fenced_rank_transport.hpp`, …), the rank-capable baselines `counted_art.hpp` and `counted_radix_ost.hpp`, the comparison driver `benchmark_equal_contract.cpp`, native benchmark drivers (`benchmark_*.cpp`) and native tests (`test_*.cpp`). Headers are included by relative name; compile with `-Ihrtli_cpp`. |
| [`hli/`](hli) | Python reference implementation (`rank_transport.py`, `signed_delta.py`, `hpsfc.py`, …). |
| [`figures/`](figures) | Scripts that regenerate every figure and Table 1 of the paper from `results/`, plus the generated PDFs. |
| [`results/`](results) | Frozen experiment records, one directory per experiment (see [`REPRODUCIBILITY.md`](REPRODUCIBILITY.md)). |
| [`scripts/`](scripts) | Corpus preparation, experiment runners, audits and verification scripts. [`scripts/modal/`](scripts/modal) holds the Modal jobs for the replicated comparison; [`scripts/as_run/`](scripts/as_run) holds byte-exact copies of runners whose hashes are recorded in experiment records. |
| [`datasets/`](datasets) | Corpus metadata (Common Crawl 200M selection: shard URLs, digests, counts). Raw corpora are not redistributed. |
| [`corpora/`](corpora) | Builders for the small public path corpora used by the Python benchmarks. |
| [`benchmarks/python/`](benchmarks/python) | Python benchmark drivers (run as modules from the repository root). |
| [`tests/`](tests) | Python unit tests. |
| [`docs/`](docs) | Theory summary, build provenance (all record digests) and detailed reproduction guides. |
| [`legacy/`](legacy) | Early exploratory prototype (hyperbolic embedding experiments); not used in the paper. |

## Requirements

* Linux x86-64 (the packed index uses POSIX memory mapping).
* C++: g++ with C++17. The replicated comparison was built with `g++ (Debian 12.2.0-14+deb12u1) 12.2.0` and
  `-std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -DHRTLI_WITH_COUNTED_ART -DHRTLI_WITH_FENCED`
  (recorded in every replicate record). Each other experiment records its own compiler and flags
  (see [`docs/BUILD_PROVENANCE.md`](docs/BUILD_PROVENANCE.md)).
* Python 3.11+ with the packages in [`requirements.txt`](requirements.txt) (`numpy`, `matplotlib`).
  Figures use `text.usetex`, so a LaTeX installation (e.g. TeX Live) is needed to regenerate them.
  Notebook audits additionally need [`requirements-audit.txt`](requirements-audit.txt).
  The Modal jobs need the `modal` client and your own Modal account.

## Quick start

```sh
git clone https://github.com/Pratz1337/RankTransport-Index.git
cd RankTransport-Index
python3 -m pip install -r requirements.txt

make verify               # SHA-256 identity of all 136 indexed experiment records
make replicated-summary   # re-audit the 10 replicate records -> replicated_summary.json
make table1               # Table 1 rows (LaTeX) from the replicate summary
make figures              # Figs. 2-6 -> figures/*.pdf
make fig1                 # Fig. 1 (TikZ) -> figures/fig1_overview.pdf
make test                 # Python unit tests

# native correctness tests (ASan/UBSan), no data needed
bash scripts/verify_radix_fenwick.sh

# build the rank-capable comparison driver and smoke-test every method (synthetic keys)
make equal-contract-smoke
```

`make paper-check` runs `verify`, `replicated-summary`, `table1`, `figures` and `fig1` in sequence.
None of these commands re-runs a timed experiment; they recompute figures and tables from the
frozen records.

## Reproducing the paper's figures and table

All commands run from the repository root. Inputs are paths under `results/`.

| Item | Content | Command | Input file(s) | Output |
|---|---|---|---|---|
| Fig. 1 | Method overview and toy run (TikZ, no data) | `make fig1` | – | `figures/fig1_overview.pdf` |
| Fig. 2 | Scale evidence on 199.9M host names: segments per bound, fit/audit time, learned vs. binary recovery, consolidation | `make figures` | `results/verification_20260905/epsilon_corrected.json`, `results/controlled_queries_20260905/analysis.json`, `results/verification_20260905/signed_consolidation_200m.json` | `figures/fig_scale.pdf` |
| Fig. 3 | Set-only workloads vs. ART, HOT, LITS (8M base, 2M arrivals) | `make figures` | `results/adaptive_competitors_20260906/summary.json` | `figures/fig_specialists.pdf` |
| Fig. 4 | Rank-capable comparison, five containers per corpus | `make replicated-summary && make figures` | `results/replicated_rankcap/repl{1..5}_*.json` → `results/replicated_rankcap/replicated_summary.json` | `figures/fig_rankcap.pdf` |
| Table 1 | Median [min, max] build, bytes/key, rank, count, mixed | `make table1` | `results/replicated_rankcap/replicated_summary.json` | LaTeX rows on stdout |
| Fig. 5 | Range-count cost vs. span: (a) earlier native index, 9.04M host keys; (b,c) replicated comparison | `make figures` | `results/benchmark_audit/range_stable_commoncrawl_host_real.json`, `results/replicated_rankcap/replicated_summary.json` | `figures/fig_range.pdf` |
| Fig. 6 | (a) ledger variants on a 200,000-key signed ledger; (b–d) earlier implementation (historical) | `make figures` | `results/adaptive_ledger_20260906/*_{linear,fenwick,adaptive}.json`, `results/early_prototype/prefix_radix_delta.json`, `results/early_prototype/cpp_benchmark_results.json` | `figures/fig_ledger.pdf` |

The figure script reads every plotted value from these files; no number is typed in by hand.
How each input was produced (driver, runner, build) is listed in
[`REPRODUCIBILITY.md`](REPRODUCIBILITY.md).

## Datasets

Raw corpora are not redistributed; every input is identified by URL and SHA-256.

**Common Crawl hosts.** Reversed host names from the Common Crawl host-level web graph release
`cc-main-2026-may-jun-jul` (240,380,987 vertex records in 48 shards).
[`datasets/commoncrawl_200m/commoncrawl_hosts_200m_metadata.json`](datasets/commoncrawl_200m/commoncrawl_hosts_200m_metadata.json)
lists all 48 shard URLs with their SHA-256 digests and the digests of the prepared files.
`scripts/prepare_commoncrawl_200m.py` verifies byte order and selects host IDs 0–199,999,999,
holding out every 2000th selected key: 199,900,000 base keys and 100,000 insertion keys
(base `6840d31d…eab7`, inserts `aff541a1…5d38`).
`scripts/prepare_lits_scale_corpora.py` derives the 10M natural-key sample used for Figs. 3, 4, 5b–c and Table 1
(source IDs divisible by 20; every fifth sampled key held out: 8,000,000 base keys and
2,000,000 arrivals; `base.txt` `c19cd9a8…43fd`, `arrivals.nul` `466ad1b9…6080`).
In the replicated runs, `scripts/modal/modal_hrtli.py::run_prepare_commoncrawl` re-downloaded all
48 shards, rebuilt both splits and asserted every digest above before benchmarking.
Full procedure: [`docs/DATA_AND_200M_REPRODUCTION.md`](docs/DATA_AND_200M_REPRODUCTION.md);
provenance record: `results/replicated_rankcap/corpus_commoncrawl.json`.

**Wikipedia titles.** English Wikipedia main-namespace titles,
`https://dumps.wikimedia.org/other/pagetitles/20260811/enwiki-20260811-all-titles-in-ns-0.gz`
(109,068,354 bytes, SHA-256 `cf23b16dc7e4091f9d13d2c2fb039de17f7979252cea4c7ec39b1d82cf41bdb8`).
19,226,329 unique keys (mean 20.97 bytes, max 255); sorted, every fifth key held out:
15,381,064 base keys and 3,845,265 arrivals (`base.txt` `4d4c225b…c613`, `arrivals.nul`
`67906855…6b8c`). Prepared by `scripts/modal/modal_hrtli.py::run_prepare_enwiki`; provenance
record: `results/replicated_rankcap/corpus_enwiki_titles.json`.

## Hardware

* **Replicated rank-capable comparison (Fig. 4, Fig. 5b–c, Table 1):** Modal containers, one fresh
  container per replicate and corpus (function request: 4 CPUs, 32 GiB). Containers were assigned
  different host CPUs: `/proc/cpuinfo` reports AuthenticAMD in nine records (family 25 models 1
  and 17, family 23 model 49) and GenuineIntel family 6 model 85 in one. Results are therefore
  reported as median and min–max over the five containers; per-run telemetry is in each record.
* **200M verification, set-only and ledger experiments (Figs. 2, 3, 6a):** one laptop under
  WSL2 (platform string recorded in each record). The controlled-query and specialist
  directories record a 13th Gen Intel Core i5-13500H with 4 logical CPUs visible and
  g++ 15.2.0 (`cpu.txt`, `compiler.txt`, `kernel.txt`, swap counters next to each experiment).
* **Earlier native index (Fig. 5a, Fig. 6b–d):** historical context only; see the limits stated in
  [`docs/BUILD_PROVENANCE.md`](docs/BUILD_PROVENANCE.md).

## Scope

HRT-LI is an in-memory, single-threaded research index, not a concurrent or crash-safe storage
engine. The certificate covers surviving base keys under serial, set-semantics writes and
non-overflowing counters; inserted keys and absent boundaries are exact but uncertified.
Consolidation is offline and blocking. Third-party baselines (libart, HOT, LITS) are not vendored;
their pinned revisions and patches are in [`docs/CURRENT_COMPARISON_REPRODUCTION.md`](docs/CURRENT_COMPARISON_REPRODUCTION.md)
and [`scripts/patches/`](scripts/patches).

## License

The authors' original code is released under the [MIT License](LICENSE). Third-party software
and Common Crawl/Wikimedia data keep their upstream terms; see
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Citation

If you use this code or these records, please cite (see also [`CITATION.cff`](CITATION.cff)):

```bibtex
@misc{nelapati2026count,
  title  = {Count, Don't Retrain: Keeping a Frozen Learned String Index Certified Under Inserts and Deletes},
  author = {Nelapati, Kshiraja and Sayal, Prathmesh},
  year   = {2026},
  note   = {Code and experiment records: https://github.com/Pratz1337/RankTransport-Index}
}
```
