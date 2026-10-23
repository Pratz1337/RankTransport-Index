"""Preliminary HRT-LI range count and scan latency benchmark.

This is not an external-baseline benchmark. It measures the current Python
RangeQueryIndex implementation so the paper can report the scan path honestly
instead of only citing the theorem.
"""

from __future__ import annotations

import json
import os
import random
import time
from typing import Callable

from hli.datasets import generate_scalable_tree
from hli.range_query import RangeQueryIndex


RESULTS_DIR = "results_q1"
RESULT_PATH = os.path.join(RESULTS_DIR, "range_latency_results.json")
BASE_KEYS = 20_000
INSERTS = 2_000
DELETES = 1_000
RANGES_PER_SELECTIVITY = 200
EPSILON = 64
SEED = 2026


def percentile(values: list[int], q: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    pos = (len(ordered) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(ordered) - 1)
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def time_call(fn: Callable[[], int]) -> tuple[int, int]:
    start = time.perf_counter_ns()
    value = fn()
    end = time.perf_counter_ns()
    return end - start, value


def build_index() -> tuple[RangeQueryIndex, list[str]]:
    rng = random.Random(SEED)
    base = sorted(set(generate_scalable_tree(BASE_KEYS, seed=SEED)))[:BASE_KEYS]
    index = RangeQueryIndex(base, EPSILON)

    inserted: list[str] = []
    for i, parent in enumerate(rng.sample(base, INSERTS)):
        key = f"{parent}/__range_ins_{i:06d}"
        if index.insert(key):
            inserted.append(key)

    for key in rng.sample(base, DELETES):
        index.delete(key)

    return index, index._rt.snapshot_keys()


def make_ranges(snapshot: list[str], target_size: int) -> list[tuple[str, str]]:
    rng = random.Random(SEED + target_size)
    if not snapshot:
        return []
    max_start = max(0, len(snapshot) - target_size)
    ranges = []
    for _ in range(RANGES_PER_SELECTIVITY):
        start = rng.randint(0, max_start)
        end = min(len(snapshot) - 1, start + target_size - 1)
        ranges.append((snapshot[start], snapshot[end]))
    return ranges


def summarize_latencies(latencies: list[int]) -> dict[str, float]:
    return {
        "p50_us": percentile(latencies, 0.50) / 1000.0,
        "p95_us": percentile(latencies, 0.95) / 1000.0,
        "p99_us": percentile(latencies, 0.99) / 1000.0,
        "avg_us": (sum(latencies) / len(latencies)) / 1000.0 if latencies else 0.0,
    }


def main() -> None:
    os.makedirs(RESULTS_DIR, exist_ok=True)
    index, snapshot = build_index()
    selectivities = [1, 10, 100, 1000]
    results: dict[str, object] = {
        "config": {
            "base_keys": BASE_KEYS,
            "inserts": INSERTS,
            "deletes": DELETES,
            "live_keys": len(snapshot),
            "mutation_count": index._rt.mutation_count,
            "ranges_per_selectivity": RANGES_PER_SELECTIVITY,
            "epsilon": EPSILON,
            "seed": SEED,
        },
        "selectivities": {},
    }

    print("Running preliminary range latency benchmark...")
    print(f"  live_keys={len(snapshot)} mutation_count={index._rt.mutation_count}")

    for target in selectivities:
        ranges = make_ranges(snapshot, target)
        count_latencies: list[int] = []
        scan_latencies: list[int] = []
        result_sizes: list[int] = []

        for lo, hi in ranges:
            count_ns, count = time_call(lambda lo=lo, hi=hi: index.count_range(lo, hi))
            scan_ns, scan_len = time_call(lambda lo=lo, hi=hi: len(index.scan_range(lo, hi)))
            if count != scan_len:
                raise AssertionError(f"range [{lo}, {hi}] count={count} scan_len={scan_len}")
            count_latencies.append(count_ns)
            scan_latencies.append(scan_ns)
            result_sizes.append(count)

        bucket = {
            "target_result_size": target,
            "avg_result_size": sum(result_sizes) / len(result_sizes),
            "count_range": summarize_latencies(count_latencies),
            "scan_range": summarize_latencies(scan_latencies),
        }
        results["selectivities"][str(target)] = bucket
        print(
            "  k~{target}: count p99={count_p99:.2f} us, scan p99={scan_p99:.2f} us".format(
                target=target,
                count_p99=bucket["count_range"]["p99_us"],
                scan_p99=bucket["scan_range"]["p99_us"],
            )
        )

    with open(RESULT_PATH, "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=2)
    print(f"Saved range latency artifact to {RESULT_PATH}")


if __name__ == "__main__":
    main()
