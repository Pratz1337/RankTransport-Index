"""Audit five fresh-process paired query runs; never treats rounds as replicates."""
from pathlib import Path
import csv
import hashlib
import json
from datetime import datetime

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "results_q1/controlled_queries_20260905"
METRICS = ("uniform_membership_ns", "balanced_membership_ns",
           "deleted_membership_ns", "uniform_rank_ns", "range_ns")


def analyze():
    records = []
    profiles = []
    hashes = {}
    for trial in range(2, 7):
        path = DATA / f"trial_{trial}.json"
        hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        record = json.loads(path.read_text())
        result = record["result"]
        assert record["exit_code"] == 0 and result["all_oracles_passed"]
        assert result["selected_keys"] == 200000000
        assert result["base_keys"] == 199900000 and result["epsilon"] == 64
        assert result["fingerprint_capacity"] == result["final_mutation_count"] == 0
        assert result["query_paths"] == "learned_binary"
        assert result["queries"] == 1000000 and result["range_queries"] == 100000
        assert result["seed"] == 20260905 and result["model_segments"] == 3008305
        assert len(result["rows"]) == 8
        assert {(row["trial"], row["mode"]) for row in result["rows"]} == {
            (i, mode) for i in range(1, 5) for mode in ("learned", "binary")}
        for row in result["rows"]:
            assert row["rank_checksum"] == 100011364934217
            assert row["range_checksum"] == 461343032
            assert all(np.isfinite(row[k]) and row[k] > 0 for k in METRICS)
        before = (DATA / f"trial_{trial}.swap_before.txt").read_text()
        after = (DATA / f"trial_{trial}.swap_after.txt").read_text()
        assert before == after, f"Guest swap changed in trial {trial}"
        log = ROOT / "results_q1/reassessment_20260905" / (
            "controlled_2_to_2.host.csv" if trial == 2 else "controlled_3_to_6.host.csv")
        with log.open(newline="", encoding="utf-8-sig") as stream:
            samples = [row for row in csv.DictReader(stream)
                       if record["started_unix"] <= datetime.fromisoformat(
                           row["Utc"].replace("Z", "+00:00")).timestamp()
                       <= record["started_unix"] + record["elapsed_seconds"]]
        assert samples
        profiles.append({
            "trial": trial, "host_samples": len(samples),
            "host_min_available_mib": min(int(r["AvailableMiB"]) for r in samples),
            "host_pageout_samples": sum(int(r["PagesOutputPerSecond"]) > 0 for r in samples),
            "host_max_pageout_per_second": max(int(r["PagesOutputPerSecond"]) for r in samples),
            "guest_swap_counter_change": False,
            **record["resource_usage"],
        })
        records.append(record)
    assert all(r["sources_sha256"] == records[0]["sources_sha256"] for r in records)
    for name, digest in records[0]["sources_sha256"].items():
        if name.startswith("scripts/"):
            source = DATA / "runtime_source" / Path(name).name
        elif name.endswith((".hpp", ".cpp")):
            source = DATA / "source" / Path(name).name
        else:
            assert digest == (DATA / "binary.sha256").read_text().split()[0]
            continue
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, source
    for line in (DATA / "source.sha256").read_text().splitlines():
        digest, name = line.split(maxsplit=1)
        source = DATA / "source" / name.removeprefix("./")
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, source
    summary = {}
    # A bootstrap draw selects whole paired processes, preserving within-process dependence.
    draws = np.random.default_rng(20260905).integers(0, 5, (10000, 5))
    for metric in METRICS:
        values = {mode: np.array([np.mean([row[metric] for row in rec["result"]["rows"]
                                         if row["mode"] == mode]) for rec in records])
                  for mode in ("learned", "binary")}
        paired_ratio = values["binary"] / values["learned"]
        ci = np.quantile(np.median(paired_ratio[draws], axis=1), [.025, .975])
        summary[metric] = {
            mode: {"per_process_mean_ns": vals.tolist(), "median_ns": float(np.median(vals)),
                   "min_ns": float(min(vals)), "max_ns": float(max(vals))}
            for mode, vals in values.items()}
        summary[metric].update({"paired_speedups": paired_ratio.tolist(),
                               "median_paired_speedup": float(np.median(paired_ratio)),
                               "paired_process_bootstrap_95": ci.tolist()})
    phases = {}
    for phase in ("construction_ms", "insert_ms", "base_delete_ms",
                  "base_reinsert_ms", "inserted_delete_ms"):
        values = [rec["result"][phase] / 1000 for rec in records]
        phases[phase] = {"seconds": values, "median_seconds": float(np.median(values)),
                         "min_seconds": min(values), "max_seconds": max(values)}
    return {"protocol": "Five fresh processes; four paired rounds averaged within each process. "
            "Reported latency is median of five process-level batch means, not median query latency. "
            "Speedup is median of five paired binary/learned ratios; 10000 process bootstrap draws, "
            "seed 20260905. All five completed trials retained, including host paging. "
            "Conditional same-machine/same-stream descriptive interval, n=5.",
            "trial_ids": list(range(2, 7)), "checked_timed_operations": 5 * 8 * 4100000,
            "source_hashes": hashes, "profiles": profiles, "metrics": summary, "phases": phases,
            "failed_launch": "trial_1: /usr/bin/time missing; benchmark did not start; no trial JSON"}


if __name__ == "__main__":
    result = analyze()
    (DATA / "analysis.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    import nbformat
    from nbclient import NotebookClient
    notebook = nbformat.v4.new_notebook()
    notebook.metadata.kernelspec = {"display_name": "Python 3", "language": "python", "name": "python3"}
    notebook.cells = [
        nbformat.v4.new_markdown_cell("""# Controlled 200M query-path evidence audit
## tl;dr
Five fresh processes passed all 164 million timed-operation checks. Median paired
binary/learned ratios are 1.287 for uniform membership, 1.278 for exact rank and
1.329 for range count. These are conditional same-state query-path comparisons,
not specialist or end-to-end index comparisons. Three processes have sampled
Windows page-out activity; all five are retained.
## Context & Methods
The experimental unit is a fresh process. Four equal-size batches per path are
averaged before summarizing five processes. A paired percentile bootstrap uses
10,000 process resamples, seed 20260905. With n=5 these descriptive intervals are
coarse and do not capture dataset, hardware, seed or host-paging uncertainty.
### Key Assumptions
Input files, source hashes and query streams are fixed. No cache flush is claimed.
Host counters describe the whole Windows machine; positive page-out is a resource
warning, not proof that a particular timed query was swapped. The first launch
failed before executing the benchmark and is documented separately.
## Data
Raw records and telemetry are retained under results_q1/controlled_queries_20260905
and results_q1/reassessment_20260905. The next cell displays and executes the exact
validation and aggregation code; it does not rerun a benchmark or start WSL.
"""),
        nbformat.v4.new_code_cell("""import sys, inspect, json
from pathlib import Path
import pandas as pd
root = Path.cwd()
assert (root / 'scripts/analyze_controlled_queries.py').is_file()
sys.path.insert(0, str(root / 'scripts'))
from analyze_controlled_queries import analyze
print(inspect.getsource(analyze))
audit = analyze()
saved = json.loads((root / 'results_q1/controlled_queries_20260905/analysis.json').read_text())
assert audit == saved
print('Raw records, checksums, source identity, guest swap and saved aggregate agree.')"""),
        nbformat.v4.new_markdown_cell("## Results\n### Process-level resource checks"),
        nbformat.v4.new_code_cell("pd.DataFrame(audit['profiles'])"),
        nbformat.v4.new_markdown_cell("### Paired query-path comparison (latency in microseconds)"),
        nbformat.v4.new_code_cell("""pd.DataFrame([{'metric': name,
    'learned_median_us': value['learned']['median_ns']/1000,
    'binary_median_us': value['binary']['median_ns']/1000,
    'median_paired_ratio': value['median_paired_speedup'],
    'paired_process_95_interval': value['paired_process_bootstrap_95']}
    for name, value in audit['metrics'].items()])"""),
        nbformat.v4.new_markdown_cell("### Mutation and construction phase times (seconds)"),
        nbformat.v4.new_code_cell("pd.DataFrame(audit['phases']).T"),
        nbformat.v4.new_markdown_cell("""## Takeaways
- High confidence: five complete records share the frozen implementation and pass
  the full driver gates. No guest swap-counter change occurred.
- High analytical risk: Windows page-out was sampled in trials 2, 3 and 6. Minimum
  available memory was 313 MiB. No process is removed after seeing its timing.
- Learned recovery has lower process-average latency for the four non-tombstone
  streams in all five processes. Deleted-key membership checks the shared ledger
  first and has no consistent learned-path advantage.
- This repairs the missing learned-versus-binary measurement. It does not supply
  a specialist learned-string baseline, independent memory comparison, natural
  mixed-write sweep, tail latency, or a replicated consolidation measurement.
- Trial 1 is a failed prelaunch, not a missing or discarded performance result.
"""),
    ]
    nbformat.validate(notebook)
    NotebookClient(notebook, timeout=90, resources={"metadata": {"path": str(ROOT)}}).execute()
    nbformat.write(notebook, DATA / "controlled_query_audit.ipynb")
