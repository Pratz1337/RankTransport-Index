# Modal jobs for the replicated rank-capable comparison (Fig. 4, Fig. 5b–c, Table 1)

The rank-capable comparison ran in [Modal](https://modal.com) containers, one fresh
container per replicate and corpus (five replicates x two corpora). Records are in
`results/replicated_rankcap/`.

| File | Role |
|---|---|
| `modal_hrtli.py` | Modal app: corpus preparation (`run_prepare_commoncrawl`, `run_prepare_enwiki`, `run_prepare_paths`), the timed session `equal_contract_session`, and exploratory `run_pilot`. Records are written to `results/replicated_rankcap/`. |
| `modal_replicate.py` | Launches the replicated sessions: `python -m modal run scripts/modal/modal_replicate.py::replicate --n 5` runs five fresh containers per corpus with the method order rotated per replicate. |
| `summarize_equal_contract.py` | Per-record audit (exit codes, `answers_checked`, certificate bound) and a quick ns/op printout. |
| `../../figures/summarize_replicates.py` | Audits all ten replicate records and writes `replicated_summary.json` (median/min/max). |

Authentication uses your own Modal account (`modal setup`); no credentials are stored here.

```sh
pip install modal
python -m modal run scripts/modal/modal_hrtli.py::run_prepare_commoncrawl
python -m modal run scripts/modal/modal_hrtli.py::run_prepare_enwiki
```

## Build recorded in every replicate record

* compiler: `g++ (Debian 12.2.0-14+deb12u1) 12.2.0`
* flags: `-std=c++17 -O3 -DNDEBUG -march=x86-64-v3 -DHRTLI_WITH_COUNTED_ART -DHRTLI_WITH_FENCED`
* driver: `hrtli_cpp/benchmark_equal_contract.cpp`, invoked as
  `equal_contract MODE BASE ARRIVALS BASE_N ARRIVAL_N DELTA QUERIES SEED EPSILON WORKLOAD`
  with `DELTA` = 80000 (hosts) / 150000 (titles), `QUERIES` = 1048576, `SEED` = 20261023, `EPSILON` = 64.

Source SHA-256 recorded in the replicate records (`build.source_sha256`). All seven files in
`hrtli_cpp/` match these digests byte for byte (`sha256sum` them to check):

| File | SHA-256 |
|---|---|
| `approx_local_delta.hpp` | `629815cf11dd90f655916d2b0522fa6ca2c9fb839ef118e46f8609c3bf277e25` |
| `benchmark_equal_contract.cpp` | `e6ec8cea1d81ecce2bf289669e99a510d9ef21fd08bf711567ccd401ba5bf557` |
| `counted_art.hpp` | `397b29c4036e1746e6ff5aa0ccd5f05dc0d4d3a74e41662debe844dae0c037e8` |
| `counted_radix_ost.hpp` | `b06bcd24c8a2484000c290f028cb36c366c937645f544cd6f6a27c3252875c2d` |
| `fenced_rank_transport.hpp` | `7a01ad9d925c77d157b3bb16c093092e183386a07577dac12eca7037f9e2a4d2` |
| `packed_rank_transport.hpp` | `ae798a132de0fa5e0cf082b364b5375270ea5b8d4eb1a1123ff61fb4ae7ab123` |
| `prefix_radix_delta.hpp` | `f10ebc9ba87468c22ea352938668b967688864e678a9ddce99d4ca8dbf8cf32c` |

## Local build and smoke test

```sh
make cpp-equal-contract     # builds hrtli_cpp/equal_contract with the flags above
make equal-contract-smoke   # synthetic 300k keys; every method must pass the driver's answer checks
```

The smoke test checks correctness only; its timings are not results.

## As-run copies

`scripts/as_run/modal_hrtli.py` is the job script exactly as it ran (paths of the original
working directory). `modal_replicate.py` was changed only in its docstring and by a
`sys.path` line so it imports `modal_hrtli.py` from this directory; the launcher as run had
SHA-256 `58227937877e2ec1cb1a1afc9146253a766d2bc4fa69231e0580b807d022a5b4`. Neither script's
digest is part of any experiment record.
