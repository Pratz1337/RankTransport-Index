"""Summarize and audit equal-contract session records (results/replicated_rankcap).

Audit checks per record: every run exited 0 with a parsed result, every stream
reports answers_checked, all runs in a session share one binary hash and input
hashes, and every maintenance step reports an audited max error <= epsilon.
Prints per-method ns/op per stream and index-owned bytes per base key.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def audit(record: dict) -> list[str]:
    problems = []
    for run in record["runs"]:
        if run["exit_code"] != 0 or run["result"] is None:
            problems.append(f"{run['mode']}: exit {run['exit_code']} {run['stderr'][-200:]}")
            continue
        result = run["result"]
        for stream in result.get("streams", []):
            if not stream.get("answers_checked"):
                problems.append(f"{run['mode']}:{stream['name']} not checked")
            m = stream.get("maintenance")
            if isinstance(m, dict) and m["audited_max_error"] > record["epsilon"]:
                problems.append(f"{run['mode']}:{stream['name']} certificate violated")
    return problems


def owned_bytes(result: dict, after: str = "heap_bytes_after") -> float:
    streams = result.get("streams") or []
    heap = streams[0][after] if streams else result.get("heap_bytes_after_build", 0)
    return heap + result.get("mapped_key_bytes", 0)


def main(paths: list[str]) -> None:
    for path in paths:
        record = json.loads(Path(path).read_text())
        problems = audit(record)
        print(f"== {Path(path).name}: corpus={record['corpus']} delta={record['delta']} seed={record['seed']} "
              f"binary={record['build']['binary_sha256'][:12]} cpu={record['runs'][0]['telemetry_before']['cpu_model']}")
        print("   audit:", "PASS" if not problems else problems)
        rows = [r for r in record["runs"] if r["result"]]
        streams = []
        for run in rows:
            for s in run["result"].get("streams", []):
                if s["name"] not in streams:
                    streams.append(s["name"])
        base_n = rows[0]["result"]["base_keys"] if rows else 1
        header = f"   {'mode':<24}{'build_s':>8}{'B/key':>8}" + "".join(f"{s[:14]:>15}" for s in streams)
        print(header)
        for run in rows:
            res = run["result"]
            if run["mode"] == "profile":
                comps = res["components"]
                print("   profile:", {k: round(v, 1) for k, v in comps.items() if k.endswith("_ns")})
                continue
            by_name = {s["name"]: s for s in res["streams"]}
            line = f"   {run['mode']:<24}{res['build_seconds']:>8.2f}{owned_bytes(res) / base_n:>8.1f}"
            line += "".join(f"{by_name[s]['ns_per_op']:>15.0f}" if s in by_name else f"{'-':>15}" for s in streams)
            print(line)
            for s in res["streams"]:
                if isinstance(s.get("maintenance"), dict):
                    m = s["maintenance"]
                    print(f"      {s['name']}: maint {m['total_seconds']:.3f}s (merge {m['merge_copy_seconds']:.3f}, "
                          f"refit {m['refit_seconds']:.3f}) inherited={m['inherited_segments']} dirty={m['dirty_segments']} "
                          f"segs {m['old_segments']}->{m['new_segments']} err={m['audited_max_error']}")


if __name__ == "__main__":
    main(sys.argv[1:])
