"""Recompute five-run descriptive summaries directly from immutable trial records."""
from pathlib import Path
import hashlib
import json
from statistics import mean, median

root = Path(__file__).resolve().parents[1]
data = root / "results_q1/controlled_queries_20260905"
metrics = {
    "uniform_membership_ns": "Uniform live membership",
    "balanced_membership_ns": "Balanced live membership",
    "deleted_membership_ns": "Deleted-base membership",
    "uniform_rank_ns": "Uniform live exact rank",
    "range_ns": "Inclusive range count",
}
records = [json.loads((data / f"trial_{i}.json").read_text()) for i in range(2, 7)]
for record in records:
    assert record["exit_code"] == 0 and record["result"]["all_oracles_passed"]
    assert len(record["result"]["rows"]) == 8
report = {"interpretation": "Descriptive median and observed range of five process means on one machine, dataset and seed. No confidence interval or population inference.",
          "trial_sha256": {f"trial_{i}.json": hashlib.sha256((data / f"trial_{i}.json").read_bytes()).hexdigest() for i in range(2, 7)},
          "metrics": {}}
for key, label in metrics.items():
    samples = {mode: [mean(row[key] for row in r["result"]["rows"] if row["mode"] == mode)
                      for r in records] for mode in ("learned", "binary")}
    paired = [b / a for a, b in zip(samples["learned"], samples["binary"])]
    report["metrics"][key] = {"per_process_mean_ns": samples, "paired_ratios": paired}
    values = [[x / 1000 for x in samples[mode]] for mode in ("learned", "binary")] + [paired]
    cells = [f"{median(v):.2f} [{min(v):.2f}, {max(v):.2f}]" for v in values]
    print(label + " & " + " & ".join(cells) + r" \\")
out = root / "results_q1/reviewer_response_20260906/query_descriptive.json"
out.write_text(json.dumps(report, indent=2) + "\n")
