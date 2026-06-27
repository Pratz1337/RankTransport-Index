# HRT-LI: Hierarchical Rank-Transport Learned Index

HRT-LI is a prototype for exact dynamic learned indexing over
hierarchical string keys such as file paths, DNS records, and JSON paths.


## Quickstart

```bash
make data
make test
make cpp-test
make cpp-benchmark
make cpp-range-benchmark
make cpp-external-smoke
python benchmark_q1.py
python benchmark_range_latency.py
```

Benchmark outputs are written to `results_q1/`.

## Key Files

- `hli/rank_transport.py` - exact dynamic rank correction layer.
- `hli/lexicode.py` - exact sparse-radix lexicographic path code.
- `hli/hpsfc.py` - expected-O(1) base-key fingerprint table, rank transport,
  and threshold-triggered consolidation.
- `hli/order_stat_tree.py` - expected `O(log n)` order-statistic treap.
- `benchmark_hrtli.py` - Q1-facing dynamic benchmark.
- `benchmark_q1.py` - compatibility entry point for `benchmark_hrtli.py`.
- `benchmark_range_latency.py` - preliminary range count/scan latency artifact.
- `download_publishable_corpora.py` - downloads redistributable URL, DNS, JSON,
  and package-manager sources and records `data_sources/SOURCES.md`.
- `export_datasets.py` - exports synthetic hierarchy, filesystem, and
  publishable URL/DNS/JSON/package-manager path corpora.
- `Makefile` - Linux/WSL targets for Python tests, data export, benchmarks,
  and native C++ smoke checks.
- `hrtli_cpp/test_rank_transport.cpp` - C++ delta-layer and consolidation smoke
  test.
- `hrtli_cpp/benchmark_range.cpp` - native range count/scan latency benchmark.
- `hrtli_cpp/benchmark_external.cpp` - optional native bindings for pinned
  ALEX/LIPP/PGM/ART/LITS/HOT checkouts under `external/competitors`.
- `hrtli_cpp/BASELINES.lock.md` - exact competitor commits and license notes.
- `hrtli_cpp/README.md` - native benchmark build commands and fairness caveats.
- `legacy_cdhli_benchmark.py` - preserved rejected Mobius-warp diagnostic.
- `Q1_HRTLI_METHODOLOGY.md` - paper-facing methodology, novelty boundary, and validation plan.
- `paper/novelty_check.md` - bounded current literature check for the exact
  rank-transport claim.

## Verified Result

On the included benchmark, stale learned predictions drift after six write
rounds, while HRT-LI keeps transported error at the original base certificate.
Inserted-key ranks are checked exactly.

| Workload | Base keys | Base epsilon | Stale max error | HRT-LI max error |
|---|---:|---:|---:|---:|
| Synthetic hierarchy | 2600 | 64 | 681 | 64 |
| Filesystem paths | 863 | 64 | 681 | 64 |
| URL paths | 502 | 64 | 674 | 64 |
| DNS hierarchy | 2880 | 64 | 681 | 64 |
| JSON paths | 2602 | 64 | 681 | 64 |
| Package paths | 704 | 64 | 681 | 64 |

URL, DNS, JSON, and package-manager rows come from the publishable corpus
builder. The current source manifest is `data_sources/SOURCES.md`.

The current Python ablation is stored in `results_q1/ablation_results.json`:

| Variant | Throughput |
|---|---:|
| Baseline | 0.383 Mops/s |
| Learned feature model only | 0.038 Mops/s |
| Feature model + transport | 0.041 Mops/s |
| HRT-LI full | 0.571 Mops/s |

The current native range benchmark is stored in
`results_q1/cpp_range_latency_synthetic.json`. With 8,000 base keys, 2,000
inserts, and 1,000 deletes, `count_range` stayed near constant latency while
`scan_range` scaled with output size:

| Target result size | count p99 | scan p99 |
|---:|---:|---:|
| 1 | 3.21 us | 0.86 us |
| 10 | 1.61 us | 1.84 us |
| 100 | 1.10 us | 11.19 us |
| 1000 | 3.67 us | 132.87 us |

The Python cross-check remains in `results_q1/range_latency_results.json`.
