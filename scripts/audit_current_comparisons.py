"""Execute the current specialist/ledger audit and produce descriptive evidence."""
from pathlib import Path
import argparse
import nbformat as nbf
from nbclient import NotebookClient

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--adaptive", action="store_true")
args = parser.parse_args()
OUT = ROOT / ("results_q1/adaptive_competitors_20260906/audit.ipynb" if args.adaptive else "results_q1/current_competitors_20260906/audit.ipynb")
nb = nbf.v4.new_notebook()
nb.metadata.kernelspec = {"display_name": "Python 3", "language": "python", "name": "python3"}
nb.cells = [
    nbf.v4.new_markdown_cell("## tl;dr\nCurrent-default specialist and direct ledger-revision evidence. The executed tables below report all predeclared cells and observed ranges; they do not infer confidence intervals or establish 200M competitive performance."),
    nbf.v4.new_markdown_cell("## Context & Methods\nTen million natural Common Crawl keys: eight million base plus two million eligible arrivals. Four distinct seeds, four methods, three workloads and fresh processes. Each cell has 1,048,576 operations. Shared independent oracle/trace, successful balanced writes, reset states, fixed CPU and rotated method order. Prefix bursts use 64 writes under `com.` alternating with 64 full-pool reads.\n\n### Key Assumptions\nThis systematic sample spans the 200M selection, not the full release. Four process/seed observations are not four datasets or machines. LITS and HOT compile separately; explicitly recorded safety patches are applied. LITS deallocation leaks are retained, not claimed fixed. Peak RSS is whole-process, not index-only. The ledger comparison isolates linear versus Fenwick sibling sums on 200,000 natural signed keys, with an independent sorted-prefix oracle."),
    nbf.v4.new_markdown_cell("## Data\nImmutable process JSON, frozen sources, protocol, dependency digests, input verification and host/swap counters reside beside this notebook. The matching ledger records are in `results_q1/current_ledger_20260906`. Failed and superseded preflight records are not benchmark measurements."),
    nbf.v4.new_code_cell('''from pathlib import Path
import json, statistics as st, csv, hashlib
from datetime import datetime
root = Path.cwd()
out = root / "results_q1/current_competitors_20260906"
ledger_dir = root / "results_q1/current_ledger_20260906"
ledger_variants = ("linear", "fenwick")
ledger_seeds = range(20260906,20260910)
inputs = json.loads((out / "verified_inputs.json").read_text())
binary_hashes = {line.split()[1]: line.split()[0] for line in (out / "binaries.sha256").read_text().splitlines()}
assert inputs["upstream_hot_and_lits_fork_compiled_separately"] and inputs["no_natural_key_truncation_or_exclusion"]
assert inputs["verified_profile"]["base.nul"]["max_key_bytes"] == 251
def describe(values):
    return {"n": len(values), "median": st.median(values), "minimum": min(values), "maximum": max(values), "observations": values}
fields = ("base_keys", "arrival_keys", "workload_family", "operations", "seed", "epsilon", "trace_fnv64", "write_percent", "reads", "read_hits", "insert_attempts", "insert_successes", "delete_attempts", "delete_successes", "final_live_keys", "max_key_bytes")
methods = ("hrtli", "art", "hot", "lits")
cells = ("readonly", "uniform50", "prefix50")
records = {}
for cell in cells:
    for seed in range(20260906, 20260910):
        paired = []
        for method in methods:
            name = f"{cell}_{seed}_{method}"
            record = json.loads((out / (name + ".json")).read_text())
            r = record["result"]
            assert record["exit_code"] == 0 and r["mode"] == method and r["seed"] == seed
            assert r["all_answers_checked"] and r["initial_and_final_states_checked"]
            assert r["operations"] == 1048576 and r["base_keys"] == r["final_live_keys"] == 8000000 and r["arrival_keys"] == 2000000
            assert r["workload_family"] == ("prefix_bursts" if cell == "prefix50" else "valid_writes")
            assert r["write_percent"] == (0 if cell == "readonly" else 50)
            count = 0 if cell == "readonly" else 262144
            assert r["insert_attempts"] == r["insert_successes"] == r["delete_attempts"] == r["delete_successes"] == count
            assert r["reads"] + 2*count == r["operations"]
            assert (out / (name + "_swap_before.txt")).read_text() == (out / (name + "_swap_after.txt")).read_text()
            sources = record["sources_sha256"]
            assert all(sources[name] == digest for name,digest in binary_hashes.items())
            for file in ("benchmark_lits_comparison.cpp", "packed_rank_transport.hpp", "prefix_radix_delta.hpp", "approx_local_delta.hpp"):
                path = out / "source_measured" / file
                matching = [v for k,v in sources.items() if k.endswith("source_measured/" + file)]
                assert matching == [hashlib.sha256(path.read_bytes()).hexdigest()]
            assert sources[(out.relative_to(root) / "verified_inputs.json").as_posix()] == hashlib.sha256((out / "verified_inputs.json").read_bytes()).hexdigest()
            records[cell, seed, method] = record
            paired.append(record)
        assert all([p["result"][f] for f in fields] == [paired[0]["result"][f] for f in fields] for p in paired)
        chronological = [p["result"]["mode"] for p in sorted(paired, key=lambda p: p["started_unix"])]
        rotation = seed - 20260906
        assert chronological == list(methods[rotation:] + methods[:rotation])
assert len(records) == 48
print("PASS: 48 planned records;", sum(r["result"]["operations"] for r in records.values()), "timed answers; 960000000 initial/final memberships")'''),
    nbf.v4.new_markdown_cell("## Results\n### Current-default specialist comparison\nSeconds per complete trace; all four observations remain visible in the JSON summary."),
    nbf.v4.new_code_cell('''summary = {"specialists": {}, "construction": {}, "whole_process_rss_kib": {}}
for cell in cells:
    summary["specialists"][cell] = {}
    for method in methods:
        result = describe([records[cell, seed, method]["result"]["workload_seconds"] for seed in range(20260906,20260910)])
        summary["specialists"][cell][method] = result
        print(cell, method, f'{result["median"]:.2f} [{result["minimum"]:.2f}, {result["maximum"]:.2f}]')
for method in methods:
    subset = [r["result"] for key,r in records.items() if key[2] == method]
    summary["construction"][method] = describe([r["build_seconds"] for r in subset])
    summary["whole_process_rss_kib"][method] = describe([r["max_rss_kib"] for r in subset])
    print(method, "construction", summary["construction"][method], "RSS KiB", summary["whole_process_rss_kib"][method])
summary["timed_answers_checked"] = sum(r["result"]["operations"] for r in records.values())
summary["initial_and_final_memberships_checked"] = 2*10000000*len(records)
summary["timed_major_faults"] = sum(r["result"]["workload_major_faults"] for r in records.values())'''),
    nbf.v4.new_markdown_cell("### Direct Fenwick revision measurement\nSame 100,000 positive and 100,000 negative natural keys; changed update order and probe stream across four seeds. Estimated ledger memory includes node/vector capacity, but not allocator metadata."),
    nbf.v4.new_code_cell('''ledger = {}
for seed in ledger_seeds:
    for variant in ledger_variants:
        name = f"{seed}_{variant}"
        record = json.loads((ledger_dir / (name + ".json")).read_text())
        r = record["result"]
        assert record["exit_code"] == 0 and r["seed"] == seed and r["variant"] == variant
        assert r["natural_signed_keys"] == 200000 and r["probe_pool"] == 400000 and r["queries"] == 1048576
        assert r["every_prefix_answer_checked"] and r["internal_invariants_checked"]
        for file in ("benchmark_ledger_revision.cpp", "prefix_radix_delta.hpp"):
            path = ledger_dir / variant / file
            assert record["sources_sha256"][path.relative_to(root).as_posix()] == hashlib.sha256(path.read_bytes()).hexdigest()
        assert (ledger_dir / (name + "_swap_before.txt")).read_text() == (ledger_dir / (name + "_swap_after.txt")).read_text()
        ledger[seed,variant] = r
    assert ledger[seed,"linear"]["trace_fnv64"] == ledger[seed,"fenwick"]["trace_fnv64"]
    assert all(ledger[seed,v]["trace_fnv64"] == ledger[seed,"linear"]["trace_fnv64"] for v in ledger_variants)
    ordered = sorted(ledger_variants, key=lambda v: json.loads((ledger_dir / f"{seed}_{v}.json").read_text())["started_unix"])
    rotation = (seed - 20260906) % len(ledger_variants)
    assert ordered == list(ledger_variants[rotation:] + ledger_variants[:rotation])
summary["ledger"] = {variant: {metric: describe([ledger[seed,variant][metric] for seed in ledger_seeds])
    for metric in ("update_seconds", "prefix_seconds", "estimated_ledger_bytes")} for variant in ledger_variants}
summary["ledger_paired_prefix_ratio"] = {v: describe([ledger[seed,"linear"]["prefix_seconds"]/ledger[seed,v]["prefix_seconds"] for seed in ledger_seeds]) for v in ledger_variants if v != "linear"}
print(json.dumps(summary["ledger"], indent=2))
print("Paired linear/Fenwick prefix ratio", summary["ledger_paired_prefix_ratio"])'''),
    nbf.v4.new_markdown_cell("### Host telemetry and historical spread\nSampled counters cannot rule out brief interference. Legacy ART/HOT point values are reconciled from all three retained process samples, not selected runs."),
    nbf.v4.new_code_cell('''summary["telemetry"] = {}
for name in ("session", "ledger_session"):
    with (out / (name + ".host.csv")).open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    times = [datetime.fromisoformat(r["Utc"].replace("Z", "+00:00")).timestamp() for r in rows]
    assert len(times) >= 2 and times == sorted(times)
    summary["telemetry"][name] = {"samples": len(rows), "minimum_available_MiB": min(int(r["AvailableMiB"]) for r in rows),
        "positive_pageout_samples": sum(float(r["PagesOutputPerSecond"]) > 0 for r in rows),
        "maximum_sampling_gap_seconds": max(b-a for a,b in zip(times,times[1:]))}
legacy = json.loads((root / "results_q1/benchmark_audit/external_benchmark_commoncrawl_host_real_art_hot.json").read_text())
summary["legacy_specialists"] = {r["name"]: {metric: describe(values) for metric,values in r["samples"].items()} for r in legacy["results"]}
print(json.dumps(summary["telemetry"], indent=2))
print(json.dumps(summary["legacy_specialists"], indent=2))
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\\n")'''),
    nbf.v4.new_markdown_cell("## Takeaways\nThese results close a current-default, multi-seed comparison at this 10M natural sample, not at 200M. Compare every workload and the ledger's measured query/update/memory trade-off before claiming an optimization. Systematic selection, one laptop, finite traces, sampled resource counters, missing index-only memory, and unrepaired baseline leaks limit interpretation. Full-release sampling, higher-scale common-domain competitors, latency/error sensitivity and replicated maintenance remain distinct research work.")
]
if args.adaptive:
    for cell in nb.cells:
        cell.source = cell.source.replace("results_q1/current_competitors_20260906", "results_q1/adaptive_competitors_20260906")
        cell.source = cell.source.replace("results_q1/current_ledger_20260906", "results_q1/adaptive_ledger_20260906")
        cell.source = cell.source.replace('ledger_variants = ("linear", "fenwick")', 'ledger_variants = ("linear", "fenwick", "adaptive")')
        cell.source = cell.source.replace('ledger_seeds = range(20260906,20260910)', 'ledger_seeds = range(20260906,20260912)')
        cell.source = cell.source.replace("with an independent sorted-prefix oracle.", "with an independent sorted-prefix oracle. The three-way ledger experiment uses six seeds and rotates method order.")
        cell.source = cell.source.replace("changed update order and probe stream across four seeds.", "changed update order and probe stream across six seeds.")
nbf.validate(nb)
NotebookClient(nb, timeout=120, kernel_name="python3", resources={"metadata": {"path": str(ROOT)}}).execute()
nbf.write(nb, OUT)
print(OUT)
