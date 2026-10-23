"""Ablation study for the HRT-LI prototype.

Components:
1. Baseline: sorted-array/B+Tree-style lookup.
2. Learned feature model only: PGM-style rank model, no transport.
3. Feature model + transport: exact dynamic correction, binary-search base path.
4. HRT-LI full: transport plus HP-SFC sparse-code fingerprint table.
"""

from __future__ import annotations

import json
import os
import random
import time

import numpy as np

from hli.baselines import BPlusTreeIndex, PiecewiseLinearIndex
from hli.datasets import generate_scalable_tree
from hli.hpsfc import HPSFCRankTransportIndex
from hli.rank_transport import RankTransportIndex


RESULTS_DIR = "results_q1"
N = 100_000
EPSILON = 64
TRIALS = 5


def measure_throughput(idx_func, keys):
    shuffled = keys.copy()
    random.shuffle(shuffled)
    times = []
    for _ in range(TRIALS):
        idx = idx_func()
        t0 = time.perf_counter()
        for key in shuffled:
            idx.lookup(key)
        times.append(len(shuffled) / (time.perf_counter() - t0) / 1e6)
    return float(np.mean(times)), float(np.std(times))


def main():
    os.makedirs(RESULTS_DIR, exist_ok=True)
    keys = sorted(set(generate_scalable_tree(N)))

    def build_baseline():
        idx = BPlusTreeIndex()
        idx.build(keys)
        return idx

    def build_feature_only():
        idx = PiecewiseLinearIndex(epsilon=EPSILON)
        idx.build(keys)
        return idx

    def build_transport_only():
        rt = RankTransportIndex(keys, EPSILON)

        class TransportLookup:
            def __init__(self):
                self.model = PiecewiseLinearIndex(epsilon=EPSILON)
                self.model.build(keys)

            def lookup(self, key):
                pred = self.model.predict_position(key)
                return rt.lookup(key, pred)

        return TransportLookup()

    def build_full():
        return HPSFCRankTransportIndex(keys, EPSILON)

    results = {}
    print("Running ablation study...")

    print("  Baseline...")
    results["Baseline"] = measure_throughput(build_baseline, keys)

    print("  Learned feature model only...")
    results["Learned feature model only"] = measure_throughput(build_feature_only, keys)

    print("  Feature model + transport...")
    results["Feature model + transport"] = measure_throughput(build_transport_only, keys)

    print("  HRT-LI full...")
    results["HRT-LI full"] = measure_throughput(build_full, keys)

    serializable = {
        name: {"mean_mops": mean, "std_mops": std}
        for name, (mean, std) in results.items()
    }

    print("\nResults (Mops/s):")
    for name, stats in serializable.items():
        print(f"  {name}: {stats['mean_mops']:.3f} +/- {stats['std_mops']:.3f}")

    with open(os.path.join(RESULTS_DIR, "ablation_results.json"), "w") as f:
        json.dump(serializable, f, indent=2)


if __name__ == "__main__":
    main()
