# Current comparison: reproduction and scope

The main specialist table uses `results_q1/adaptive_competitors_20260906`.
The main ledger table uses `results_q1/adaptive_ledger_20260906`.
Paths below are relative to the repository root.
Do not pool these records with the all-node prototype in
`results_q1/current_competitors_20260906` or the earlier LITS pilots.

## Audit without rerunning benchmarks

With Python 3, `nbformat`, `nbclient`, `ipykernel`, and a Python 3 Jupyter kernel:

```text
python scripts/audit_current_comparisons.py --adaptive
```

This executes the notebook, verifies all 48 declared specialist records and
18 ledger records, compares traces and source identities, and regenerates
`summary.json`. That file retains every observation, construction/RSS values,
host telemetry, and the three-process historical ART/HOT ranges. No confidence
interval is inferred from the four or six observations.

## Inputs and frozen sources

Prepare the verified 200M source using the included Common Crawl metadata and
preparation program. Then run:

```text
python scripts/prepare_lits_scale_corpora.py PATH_TO_PREPARED_200M NEW_DESTINATION
```

Use `NEW_DESTINATION/sample_10000000`. Its `base.txt`, `base.nul`, and
`arrivals.nul` must match the hashes in the current `verified_inputs.json`.
The parent source is streamed, ordered and hash checked by preparation. The
sample takes source IDs divisible by 20; every fifth sampled key is held out.
It is not randomly sampled and does not cover the complete 240M release.

The measured C++ sources are under the current record directory's
`source_measured/`. Each ledger variant has its own frozen header and identical
driver. Retain those files when recreating an experiment; using today's source
does not reproduce a historical implementation.

## External sources, not redistributed

Acquire these upstream revisions separately:

| Repository | Revision |
|---|---|
| https://github.com/armon/libart | `301046804af165269e37da6725f5a4aec9ecc881` |
| https://github.com/speedskater/hot | `96bf6fb7103b27e50e16a6026db8974c090ee84a` |
| https://github.com/schencoding/lits | `6f4793dff0dcf66daada49e2f4ae1b1838bfa9cc` |

Export libart and HOT at these revisions; export the LITS repository's `LITS/`
subdirectory. Apply the packaged `hot-portable-mask.patch` at the HOT root.
At the exported `LITS/` root apply, in order:

1. `lits-pmss-bounds.patch`
2. `lits-hot-portability.patch`
3. `lits-iterator-root.patch`
4. `lits-hot-iterator-assignment.patch`
5. `lits-hot-empty-mask.patch`
6. `lits-zero-prefix-context.patch`

ART remains unmodified. HOT patches address unaligned loads, negative byte
offsets and zero masks. LITS patches address bounds, iterator and mask behavior.
Adversarial adapter records document checks and failures. A retained LITS test
reports 323,695 leaked bytes in 9,224 allocations. The subsequent address/UB
test disables leak checking explicitly; it is not a leak-clean result.

Build commands, compiler version, CPU information and dependency manifests are
packaged. The build scripts record the original laptop's absolute paths and
temporary directories: they are execution records, not a portable one-command
installer. Relocate those paths to your exported sources and new output
directory before use. `build_current_competitors.sh` documents the optimized
libart object and isolated builds; `build_adaptive_competitors.sh` uses the
current frozen header. GCC/G++ used `-O3 -DNDEBUG -march=native`, C11/C++17.

IMPORTANT: compile ART/HOT with `HRTLI_WITH_ART_HOT` and without LITS includes;
compile LITS separately without upstream HOT includes. The LITS project embeds
a HOT fork with colliding namespaces/header guards. The recorded dependency
manifests verify isolation. A new compiler or build path can change binary
digests; preserve new identities instead of asserting a historical hash.

## Run protocol

`source_measured/run_current_competitors.sh` specifies all 48 invocations:
four methods, seeds 20260906 through 20260909, three workloads, 1,048,576
operations, epsilon 64, CPU 2, rotated method order. The binary arguments are:

```text
MODE BASE_TXT BASE_NUL ARRIVALS_NUL 8000000 2000000 1048576 WRITE_PERCENT SEED 64 FAMILY
```

Families are `valid_writes` (0 or 50 percent writes) and `prefix_bursts`
(50 percent). Every requested write succeeds using active/inactive pools;
the prefix case alternates 64 writes under `com.` with 64 full-pool reads.
`com.` covers 60.8% of this sample, not an extreme narrow hotspot. Every timed
answer and the complete initial/final membership state is independently checked.
No input key is excluded or truncated; the 251-byte maximum fits HOT's 254-byte
domain. The larger 200M corpus is not wholly within that domain.

`run_adaptive_ledger.sh` specifies the three ledger variants, six seeds, rotated
order and independent prefix oracle. Its binaries take BASE_TXT, ARRIVALS_NUL,
SEED and VARIANT. The fixture contains 100k negative and 100k positive natural
keys and 400k boundary probes; 1,048,576 prefix queries are checked per process.

## Resource and interpretation limits

The current specialist session has 74 host samples, seven positive page-out
samples and an 80 MiB minimum available-memory sample. Guest timed major faults
are zero and swap counters unchanged; neither fact rules out Windows host
interference. Keep all runs. Peak RSS includes benchmark-owned data and build
temporaries. LITS leaks remain in this finite-trace measurement. The ledger
session has no sampled page-outs, but is still one reused dataset and machine.
The fanout threshold 16 is an engineering choice, not a held-out tuned optimum.

These records support exact answers and bounded descriptive comparisons, not
200M competitive throughput, steady-state memory, or portable latency claims.
