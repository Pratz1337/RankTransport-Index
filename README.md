# HRT-LI: Certified Rank Transport

Research code and reproducibility records for **Certified rank transport for
hierarchical strings**, by Prathmesh Sayal and Kshiraja Nelapati.

HRT-LI separates a frozen learned base from a signed prefix-mass ledger. For a
query key `k`, the ledger stores the exact change in the number of live keys
strictly below `k`:

```text
rank_t(k) = rank_0(k) + Delta_t(k)
p_t(k)    = p_0(k)    + Delta_t(k)
```

For surviving base keys, applying the same correction preserves the base
prediction's certified error. Inserted keys and absent query boundaries use
exact recovery; they do not inherit the stored-base prediction certificate.
The set contract uses unsigned-byte lexicographic order and excludes duplicates.

## Implementation

The serial C++17 implementation is in
[`hrtli_cpp/packed_rank_transport.hpp`](hrtli_cpp/packed_rank_transport.hpp), with
the compressed radix ledger in
[`hrtli_cpp/prefix_radix_delta.hpp`](hrtli_cpp/prefix_radix_delta.hpp).
The default query path uses guarded learned recovery without the optional
fingerprint table. The refined implementation includes segment-fence recovery
and fanout-aware sibling accounting: direct sums at up to 16 children and a
Fenwick array above that threshold. These are distinct from the earlier
implementation measured in the 200M-key query trials.

The packed implementation requires Linux/POSIX facilities such as memory mapping.
The newline-delimited base-file format cannot represent a newline within a key.
This is an in-memory, single-threaded research index, not a concurrent or
crash-safe storage engine. Offline consolidation is blocking.

The Python reference implementations are in `hli/rank_transport.py`,
`hli/signed_delta.py`, and `hli/hpsfc.py`.

## Match each result to its build

| Evidence | Location | Scope |
|---|---|---|
| 200M selected Common Crawl keys; five process trials | [`controlled_queries_20260905`](results_q1/controlled_queries_20260905) | Learned-versus-binary recovery inside the same index; 164M timed answers checked. Uses linear sibling sums and predates segment-fence refinement. |
| Refined HRT-LI versus ART, HOT and LITS | [`adaptive_competitors_20260906`](results_q1/adaptive_competitors_20260906) | 10M natural keys: 8M base and 2M eligible arrivals; four seeds, three workloads, 48 processes. Membership contract, not rank/range comparison. |
| Linear, all-node Fenwick and fanout-aware ledgers | [`adaptive_ledger_20260906`](results_q1/adaptive_ledger_20260906) | 200K natural signed keys, six seeds and 18 processes. Query time, update time and estimated ledger memory. |
| Error-budget sweep and offline consolidation | [`reassessment_20260905`](results_q1/reassessment_20260905) | Separate frozen builds and verification records; consolidation timing is one observation. |

The [build provenance table](docs/BUILD_PROVENANCE.md) and
[full source/binary digest map](docs/BUILD_PROVENANCE.json) identify experiment-time
sources. Use the corresponding frozen `source/`, `source_measured/`, or variant
directory to reproduce a measured build. A newly compiled binary need not have
the historical binary digest. The repository commit identifies this release;
it is not retroactively the commit used by those experiments.

The 200M experiment is an internal ablation, not a competitive benchmark.
In the refined membership comparison, ART, HOT and LITS finish every measured
workload faster than HRT-LI. The fanout-aware ledger has overlapping observed
query-time ranges with linear sums and higher estimated memory use. These
measurements do not establish universal throughput superiority.

Timing summaries use medians and observed ranges, not population confidence
intervals. The experiments use one laptop and systematic selections from one
Common Crawl release. Windows-host memory pressure is retained in the telemetry;
the records do not establish portable latency or steady-state service behavior.

## Lightweight checks: no corpus download or benchmark rerun

From the repository root, with Python 3.11:

```sh
python -m pip install -r requirements-audit.txt
python -m ipykernel install --user --name python3
python scripts/verify_release.py
python -m unittest tests.test_rank_transport tests.test_hpsfc tests.test_consolidation_failure tests.test_benchmark_hrtli_audit tests.test_signed_delta_transactions tests.test_hpsfc_automatic_failure tests.test_dataset_seed
python scripts/audit_current_comparisons.py --adaptive
python scripts/summarize_query_runs_descriptively.py
```

The notebook audit validates all 48 specialist records, 18 ledger records, source
identities and shared traces, and regenerates the descriptive summary. The final
command recomputes the five-process query summaries. These commands inspect
recorded experiments; they do not independently rerun their timed operations.
Audit scripts regenerate the notebook/summary files, so run them in a disposable
checkout if you want to preserve the release checkout byte-for-byte.

For the current native correctness/sanitizer fixtures on Linux with GCC/G++:

```sh
bash scripts/verify_radix_fenwick.sh
```

The focused Python suite and both record-summary audits were rerun for this
release. Native sanitizer and full-scale performance reruns were not performed
during release packaging; their retained records are dated separately.

## Reproduce the real-corpus experiments

See [data and 200M-query reproduction](docs/DATA_AND_200M_REPRODUCTION.md) and
[specialist/ledger reproduction](docs/CURRENT_COMPARISON_REPRODUCTION.md).
They identify source URLs, selection rules, input checksums, pinned third-party
revisions, patches, build separation, operation streams and resource limits.

The multi-gigabyte Common Crawl corpus and third-party baseline checkouts are
not redistributed. The repository includes all 48 upstream shard URLs and
checksums, preparation code, frozen HRT-LI sources, process-level measurements,
host telemetry, and audit notebooks.

## Historical material

Existing root-level exploratory drivers, `analysis/`, `results_dynamic/`, older
undated `results_q1/` files and the legacy `Makefile` are retained for history.
They are not the reproduction entry points for the manuscript's main tables.
The dated `current_competitors_20260906` directory contains the superseded
all-node-Fenwick prototype, despite its historical name; the refined results
are in `adaptive_competitors_20260906`.

The historical `controlled_query_audit.ipynb` and
`scripts/analyze_controlled_queries.py` retain an earlier bootstrap analysis.
Use `scripts/summarize_query_runs_descriptively.py` and
`results_q1/reviewer_response_20260906/query_descriptive.json` for the manuscript's
five-run descriptive summaries. Failed/preflight diagnostics remain labelled
and must not be counted as successful performance trials.

## License

The authors' original HRT-LI code is released under the [MIT License](LICENSE).
Third-party software and Common Crawl data retain their upstream terms; see
[third-party notices](THIRD_PARTY_NOTICES.md).
