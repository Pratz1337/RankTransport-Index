#!/usr/bin/env python3
"""Audit and summarise the replicated rank-capable comparison.

Input : results/replicated_rankcap/repl{1..5}_*.json
        (one fresh Modal container per replicate and corpus; same trace, seed,
        epsilon and query count as the pilot; method order rotated per replicate).
Output: results/replicated_rankcap/replicated_summary.json

Parsing follows scripts/modal/summarize_equal_contract.py:
  * a run is valid if exit_code == 0 and result is not None;
  * every stream must report answers_checked == True;
  * any maintenance block must have audited_max_error <= epsilon;
  * bytes per key = (streams[0].heap_bytes_after + mapped_key_bytes) / base_keys.
Additionally checked here: one binary hash and one set of input hashes per
corpus across all replicates, identical base_keys / segments per corpus.
Statistics: median, min, max over replicates (n = number of valid replicates).
Usage: python3 figures/summarize_replicates.py [REPO_ROOT]
"""
import glob, json, os, re, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, ".."))
D = os.path.join(R, "results", "replicated_rankcap")
OUT = os.path.join(D, "replicated_summary.json")


def owned_bytes(res):
    streams = res.get("streams") or []
    heap = streams[0]["heap_bytes_after"] if streams else res.get("heap_bytes_after_build", 0)
    return heap + res.get("mapped_key_bytes", 0)


def audit(rec):
    problems = []
    for run in rec["runs"]:
        if run["exit_code"] != 0 or run["result"] is None:
            problems.append(f"{run['mode']}: exit {run['exit_code']}")
            continue
        for s in run["result"].get("streams", []):
            if not s.get("answers_checked"):
                problems.append(f"{run['mode']}:{s['name']} not checked")
            m = s.get("maintenance")
            if isinstance(m, dict) and m["audited_max_error"] > rec["epsilon"]:
                problems.append(f"{run['mode']}:{s['name']} certificate violated")
    return problems


def stats(vals):
    return {"median": st.median(vals), "min": min(vals), "max": max(vals), "n": len(vals), "values": vals}


def main():
    files = sorted(glob.glob(os.path.join(D, "repl[0-9]*_*.json")))
    by_corpus, audits = {}, []
    for f in files:
        rec = json.load(open(f))
        probs = audit(rec)
        runs = rec["runs"]
        cpus = sorted({"family %s model %s (%s)" % (r["telemetry_before"]["cpu_fields"].get("cpu family"),
                                                   r["telemetry_before"]["cpu_fields"].get("model"),
                                                   r["telemetry_before"]["cpu_fields"].get("vendor_id")) for r in runs})
        n_streams = sum(len(r["result"]["streams"]) for r in runs if r["result"])
        audits.append({"file": os.path.basename(f), "corpus": rec["corpus"], "replicate": rec.get("replicate"),
                       "modes_in_order": rec["modes_in_order"], "binary_sha256": rec["build"]["binary_sha256"],
                       "runs": len(runs), "streams_checked": n_streams, "cpus": cpus,
                       "status": "PASS" if not probs else "FAIL", "problems": probs})
        by_corpus.setdefault(rec["corpus"], []).append((rec, probs))

    summary = {"source_dir": os.path.relpath(D, R), "files": [a["file"] for a in audits], "audit": audits,
               "consistency": {}, "corpora": {}}
    for corpus, recs in by_corpus.items():
        valid = [r for r, p in recs if not p]
        summary["consistency"][corpus] = {
            "binaries": sorted({r["build"]["binary_sha256"] for r, _ in recs}),
            "inputs_identical": len({json.dumps(r["inputs_sha256"], sort_keys=True) for r, _ in recs}) == 1,
            "delta": sorted({r["delta"] for r, _ in recs}), "seed": sorted({r["seed"] for r, _ in recs}),
            "epsilon": sorted({r["epsilon"] for r, _ in recs}), "queries": sorted({r["queries"] for r, _ in recs}),
            "segments": sorted({run["result"]["model_segments"] for r, _ in recs for run in r["runs"]
                                if run["result"] and "model_segments" in run["result"]}),
            "base_keys": sorted({run["result"]["base_keys"] for r, _ in recs for run in r["runs"] if run["result"]}),
            "valid_replicates": len(valid), "total_replicates": len(recs)}
        per_mode = {}
        for rec in valid:
            for run in rec["runs"]:
                res = run["result"]
                row = per_mode.setdefault(run["mode"], {})
                row.setdefault("build_seconds", []).append(res["build_seconds"])
                row.setdefault("bytes_per_key", []).append(owned_bytes(res) / res["base_keys"])
                if "max_rss_kib" in res:
                    row.setdefault("peak_rss_mib", []).append(res["max_rss_kib"] / 1024)
                row.setdefault("wall_seconds", []).append(run["wall_seconds"])
                for s in res["streams"]:
                    row.setdefault("ns_per_op:" + s["name"], []).append(s["ns_per_op"])
        summary["corpora"][corpus] = {m: {k: stats(v) for k, v in row.items()} for m, row in per_mode.items()}
        # paired (same container) ratios, one value per replicate
        paired = {}
        for rec in valid:
            res = {run["mode"]: run["result"] for run in rec["runs"]}
            ns = {m: {s["name"]: s["ns_per_op"] for s in r["streams"]} for m, r in res.items()}
            h = "hrtli"
            for stream in ns[h]:
                paired.setdefault("hrtli_binary_over_hrtli:" + stream, []).append(ns["hrtli_binary"][stream] / ns[h][stream])
                for m in ("counted_art", "radix_ost", "pbds_rbtree"):
                    paired.setdefault(f"{m}_over_hrtli:" + stream, []).append(ns[m][stream] / ns[h][stream])
            for m in ("counted_art", "radix_ost", "pbds_rbtree"):
                paired.setdefault(f"{m}_over_hrtli:build_seconds", []).append(res[m]["build_seconds"] / res[h]["build_seconds"])
                paired.setdefault(f"{m}_over_hrtli:bytes_per_key", []).append(owned_bytes(res[m]) / owned_bytes(res[h]))
        summary["corpora"][corpus]["_paired_ratios"] = {k: stats(v) for k, v in paired.items()}
    with open(OUT, "w") as f:
        json.dump(summary, f, indent=1)
    for a in audits:
        print(a["file"], a["status"], a["streams_checked"], "streams", a["cpus"], a["problems"])
    print(json.dumps(summary["consistency"], indent=1))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
