"""Compute streaming descriptive statistics for the audited 200M host keys."""

from __future__ import annotations

import argparse
import collections
import heapq
import json
from pathlib import Path


def quantile(histogram: collections.Counter[int], count: int, q: float) -> int:
    target = max(1, int(q * count + 0.999999999))
    seen = 0
    for value in sorted(histogram):
        seen += histogram[value]
        if seen >= target:
            return value
    raise RuntimeError("empty histogram")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("initial", type=Path)
    parser.add_argument("inserts", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    lengths: collections.Counter[int] = collections.Counter()
    depths: collections.Counter[int] = collections.Counter()
    top_labels: collections.Counter[str] = collections.Counter()
    count = total_length = total_depth = 0
    first_key = last_key = previous = None

    with args.initial.open("rb") as initial_source, args.inserts.open("rb") as insert_source:
        for raw in heapq.merge(initial_source, insert_source):
            key = raw.rstrip(b"\n")
            if not key:
                raise SystemExit("empty key in selected corpus")
            if previous is not None and previous >= key:
                raise SystemExit(f"non-increasing key order at record {count}")
            if first_key is None:
                first_key = key.decode("utf-8")
            previous = key
            last_key = key.decode("utf-8")
            length = len(key)
            depth = key.count(b".") + 1
            lengths[length] += 1
            depths[depth] += 1
            top_labels[key.split(b".", 1)[0].decode("utf-8")] += 1
            count += 1
            total_length += length
            total_depth += depth

    if count < 200_000_000:
        raise SystemExit(f"expected at least 200M keys, found {count}")

    payload = {
        "dataset": "Common Crawl host-level Web Graph May-June-July 2026",
        "records": count,
        "first_key": first_key,
        "last_key": last_key,
        "strict_bytewise_order_verified": True,
        "key_length_bytes": {
            "mean": total_length / count,
            "median": quantile(lengths, count, 0.50),
            "p90": quantile(lengths, count, 0.90),
            "p95": quantile(lengths, count, 0.95),
            "p99": quantile(lengths, count, 0.99),
            "max": max(lengths),
            "histogram": dict(sorted(lengths.items())),
        },
        "hierarchy_depth": {
            "mean": total_depth / count,
            "median": quantile(depths, count, 0.50),
            "p90": quantile(depths, count, 0.90),
            "p95": quantile(depths, count, 0.95),
            "p99": quantile(depths, count, 0.99),
            "max": max(depths),
            "histogram": dict(sorted(depths.items())),
        },
        "top_reverse_domain_labels": [
            {"label": label, "keys": value, "fraction": value / count}
            for label, value in top_labels.most_common(20)
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(
        f"PASS records={count} mean_length={total_length / count:.3f} "
        f"mean_depth={total_depth / count:.3f}"
    )


if __name__ == "__main__":
    main()
