"""Build and execute a read-only audit notebook for the legacy compact evidence."""
from pathlib import Path
import nbformat
from nbclient import NotebookClient

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "results_q1/reassessment_20260905/compact_evidence_audit.ipynb"
notebook = nbformat.v4.new_notebook()
notebook.metadata.kernelspec = {"display_name": "Python 3", "language": "python", "name": "python3"}
notebook.cells = [
    nbformat.v4.new_markdown_cell("""## tl;dr
The five compact workloads have **different input-path and indexed-base counts**.
The historical records cover generated suffix mutations, not natural arrivals.
Their old independent-looking insertion check was limited to 64 keys and used an
index-provided snapshot. Do not upgrade those old records to the corrected
oracle merely because the current driver is fixed.

## Context & Methods
This notebook audits existing files, without downloads, WSL, or performance runs.
The paper is the primary artifact; this notebook is its inspectable audit trail.

### Key Assumptions
The recorded `current_size`, `inserted_total`, and `deleted_base_total` fields have
their driver-defined set-cardinality meaning. Infer the original indexed base as
`current_size - inserted_total + deleted_base_total` and require all six rounds
to agree. This is a reconciliation of historical records, not an independent
reproduction of the original source extraction. Depth and byte-length statistics
describe input paths, not the prefix-closed base. Generated regression keys below
are software tests only, never real-corpus performance evidence.
"""),
    nbformat.v4.new_markdown_cell("## Data\nRecord the exact files used by this audit."),
    nbformat.v4.new_code_cell('''from pathlib import Path
import hashlib
import json
import pandas as pd
root = Path.cwd()
paths = {
    "legacy_results": root / "results_q1/hrtli_benchmark_results.json",
    "corrected_driver": root / "benchmark_hrtli.py",
    "regression_record": root / "results_q1/reassessment_20260905/compact_oracle_regression.json",
}
source_hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}
pd.DataFrame([{"source": name, "path": str(paths[name].relative_to(root)), "sha256": digest}
              for name, digest in source_hashes.items()])'''),
    nbformat.v4.new_markdown_cell("## Results\n### Reconcile input paths with indexed-base cardinality"),
    nbformat.v4.new_code_cell('''legacy = json.loads(paths["legacy_results"].read_text(encoding="utf-8"))
rows = []
for name in ("Filesystem paths", "URL paths", "DNS hierarchy", "JSON paths", "Package paths"):
    result = legacy[name]
    history = result["history"]
    assert [row["round"] for row in history] == list(range(1, 7))
    counts = {row["current_size"] - row["inserted_total"] + row["deleted_base_total"] for row in history}
    assert len(counts) == 1, (name, counts)
    base = counts.pop()
    input_count = result["dataset_stats"]["num_keys"]
    rows.append({"workload": name, "input_paths": input_count, "indexed_base": base,
                 "extra_indexed_keys": base - input_count,
                 "round6_insertions": history[-1]["inserted_total"],
                 "round6_base_deletions": history[-1]["deleted_base_total"],
                 "requested_epsilon": result["base_model"]["target_epsilon"],
                 "realized_epsilon": result["base_model"]["epsilon"]})
profile = pd.DataFrame(rows)
assert (profile["indexed_base"] > profile["input_paths"]).all()
profile'''),
    nbformat.v4.new_markdown_cell("### Verify the new guard tests actually ran\nA passing current test does not relabel historical data as newly validated."),
    nbformat.v4.new_code_cell('''regression = json.loads(paths["regression_record"].read_text(encoding="utf-8"))
assert regression["exit_code"] == 0
assert "Ran 7 tests" in regression["stderr"] and "OK" in regression["stderr"]
assert regression["sources_sha256"]["benchmark_hrtli.py"] == source_hashes["corrected_driver"]
print(regression["stderr"])
print("All five historical input/base counts differ; the original round-level cardinalities reconcile.")'''),
    nbformat.v4.new_markdown_cell("""## Takeaways
- **High severity, high confidence:** mislabeled grain understates indexed
  cardinality, especially package paths. Report both counts and identify which
  population the length/depth statistics describe.
- **High severity, high confidence (driver inspection):** generated suffix
  arrivals and prefix closure make these constructed correctness fixtures.
  They are not natural workload timing evidence.
- **High severity, high confidence (driver inspection):** the old rank check
  sampled only 64 insertions from an index snapshot; other exhaustive comparisons
  used the same implementation's exact-rank method. The corrected driver checks
  every live rank against an input-derived set and rejects certificate overflow.
- **Medium severity, high confidence:** the historical update reference began
  HRT-LI after six mutation rounds but built other methods fresh. The driver now
  constructs a fresh HRT-LI state; no old timing is thereby rehabilitated.

No temporal trend is inferred from these single saved artifacts. No new public
dataset extraction was performed, and the original filesystem snapshot has not
been independently reconstructed. Controlled full-scale and specialist-baseline
performance evidence remains pending.
"""),
]
nbformat.validate(notebook)
NotebookClient(notebook, timeout=90, resources={"metadata": {"path": str(ROOT)}}).execute()
nbformat.write(notebook, OUT)
print(f"Executed audit: {OUT}")
