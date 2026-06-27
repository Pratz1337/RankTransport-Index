"""
measure_trie_depth.py — Critical prerequisite measurement for HP-Delta decision.

The expert's exact instruction:
  "Before you invest weeks implementing HP-Delta, measure the actual
   compressed-trie branching depth distribution on your six datasets
   and the empirical a(k) under your workloads. If the median compressed
   depth is, say, 12 and log m is 20, you have a real ~1.6x plus
   cache-locality win worth publishing. If it's 30, the theorem is
   vacuous on your own data."

This script builds a compressed Patricia trie over each dataset's sorted
keys and reports:
  - Compressed branching depth per key (= number of branching nodes on path)
  - Distribution: min, p25, median, p75, p95, max
  - Compare to log2(m) for mutation counts m we actually measured
"""

import os
import sys
import math
import statistics
import json
from dataclasses import dataclass, field


# ── Compressed (Patricia) Trie implementation ────────────────────────────────

@dataclass
class TrieNode:
    edge: bytes = b""
    children: dict = field(default_factory=dict)
    is_leaf: bool = False


def build_compressed_trie(sorted_keys):
    root = TrieNode(edge=b"")
    for key in sorted_keys:
        kb = key.encode("utf-8") + b"\x00"
        _insert(root, kb, 0)
    return root


def _insert(node, key, depth):
    if depth == len(key):
        node.is_leaf = True
        return

    first = key[depth:depth+1]

    if first not in node.children:
        child = TrieNode(edge=key[depth:])
        child.is_leaf = True
        node.children[first] = child
        return

    child = node.children[first]
    edge = child.edge

    rem = key[depth:]
    common = 0
    while common < len(edge) and common < len(rem) and edge[common] == rem[common]:
        common += 1

    if common == len(edge):
        _insert(child, key, depth + common)
        return

    # Split edge at common
    new_branch = TrieNode(edge=edge[:common])
    child.edge = edge[common:]
    new_branch.children[edge[common:common+1]] = child

    if common == len(rem):
        new_branch.is_leaf = True
    else:
        new_leaf = TrieNode(edge=rem[common:])
        new_leaf.is_leaf = True
        new_branch.children[rem[common:common+1]] = new_leaf

    node.children[first] = new_branch


def compressed_branching_depth(root, key):
    """
    Returns the number of branching nodes (nodes with >1 child)
    on the root-to-leaf path for key.
    This is the 'compressed branching depth' the expert refers to.
    """
    kb = key.encode("utf-8") + b"\x00"
    node = root
    depth_ptr = 0
    branching_count = 0

    while depth_ptr < len(kb):
        first = kb[depth_ptr:depth_ptr+1]
        if first not in node.children:
            break
        child = node.children[first]

        if len(node.children) > 1:
            branching_count += 1

        edge = child.edge
        rem = kb[depth_ptr:]
        common = 0
        while common < len(edge) and common < len(rem) and edge[common] == rem[common]:
            common += 1

        depth_ptr += common
        node = child

        if common < len(edge):
            break

    return branching_count


def measure_dataset(name, initial_path, insert_path, max_keys=30000):
    print(f"\n{'='*60}")
    print(f"Dataset: {name}")
    print(f"{'='*60}")

    with open(initial_path, "r", encoding="utf-8", errors="ignore") as f:
        initial_keys = [line.strip() for line in f if line.strip()]
    with open(insert_path, "r", encoding="utf-8", errors="ignore") as f:
        insert_keys = [line.strip() for line in f if line.strip()]

    if len(initial_keys) > max_keys:
        step = len(initial_keys) // max_keys
        initial_keys = initial_keys[::step][:max_keys]

    all_keys = sorted(set(initial_keys))
    all_keys = all_keys[:max_keys]
    m_mutations = min(len(insert_keys), len(initial_keys) // 5)
    log_m = math.log2(max(m_mutations, 2))

    print(f"  Keys sampled : {len(all_keys):,}")
    print(f"  Mutations (m): {m_mutations:,}")
    print(f"  log2(m)      : {log_m:.1f}  <- Fenwick/treap cost per query")

    key_lengths = [len(k) for k in all_keys]
    median_len  = statistics.median(key_lengths)
    print(f"  Key length   : min={min(key_lengths)} median={median_len:.0f} max={max(key_lengths)} bytes")
    print(f"  Uncompressed D_k = median key length = {median_len:.0f} "
          f"(would be {'WORSE' if median_len > log_m else 'better'} than log2(m)={log_m:.1f})")

    print(f"  Building compressed Patricia trie...")
    root = build_compressed_trie(all_keys)
    print(f"  Trie built. Measuring branching depths...")

    # Sample 4000 keys evenly
    sample = all_keys[::max(1, len(all_keys)//4000)]
    depths = [compressed_branching_depth(root, k) for k in sample]

    ds = sorted(depths)
    n  = len(ds)
    d_min = ds[0]
    d_p25 = ds[n//4]
    d_med = statistics.median(depths)
    d_p75 = ds[3*n//4]
    d_p95 = ds[int(0.95*n)]
    d_max = ds[-1]

    speedup = log_m / d_med if d_med > 0 else float('inf')
    if d_med < log_m * 0.65:
        verdict = "HP-DELTA WINS"
    elif d_med < log_m:
        verdict = "MARGINAL"
    else:
        verdict = "HP-DELTA LOSES"

    print(f"\n  COMPRESSED BRANCHING DEPTH DISTRIBUTION:")
    print(f"    min={d_min}  p25={d_p25}  median={d_med:.1f}  p75={d_p75}  p95={d_p95}  max={d_max}")
    print(f"    log2(m) = {log_m:.1f}")
    print(f"    Speedup estimate: {speedup:.2f}x  =>  {verdict}")

    return {
        "dataset": name,
        "n_keys": len(all_keys),
        "m_mutations": m_mutations,
        "log_m": round(log_m, 2),
        "key_length_median": round(float(median_len), 1),
        "compressed_depth_min": d_min,
        "compressed_depth_p25": d_p25,
        "compressed_depth_median": round(float(d_med), 2),
        "compressed_depth_p75": d_p75,
        "compressed_depth_p95": d_p95,
        "compressed_depth_max": d_max,
        "speedup_estimate": round(speedup, 2),
        "verdict": verdict,
    }


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.path.join(base, "data")

    datasets = [
        ("synthetic",  "synthetic_initial.txt",  "synthetic_insert.txt"),
        ("filesystem", "filesystem_initial.txt",  "filesystem_insert.txt"),
        ("url",        "url_initial.txt",          "url_insert.txt"),
        ("dns",        "dns_initial.txt",           "dns_insert.txt"),
        ("json",       "json_initial.txt",          "json_insert.txt"),
        ("package",    "package_initial.txt",       "package_insert.txt"),
    ]

    results = []
    for name, init_f, ins_f in datasets:
        ip = os.path.join(data_dir, init_f)
        xp = os.path.join(data_dir, ins_f)
        if not os.path.exists(ip):
            print(f"[SKIP] {name}: {ip} not found")
            continue
        try:
            r = measure_dataset(name, ip, xp)
            results.append(r)
        except Exception as e:
            import traceback
            print(f"[ERROR] {name}: {e}")
            traceback.print_exc()

    print(f"\n{'='*70}")
    print("SUMMARY — Go/No-Go Decision for HP-Delta Implementation")
    print(f"{'='*70}")
    print(f"{'Dataset':<14} {'log2(m)':>8} {'ByteDepth':>10} {'CompDepth':>10} {'Speedup':>8}  Verdict")
    print("-"*70)
    wins = 0
    for r in results:
        print(f"{r['dataset']:<14} {r['log_m']:>8.1f} {r['key_length_median']:>10.0f} "
              f"{r['compressed_depth_median']:>10.1f} {r['speedup_estimate']:>8.2f}x  {r['verdict']}")
        if r['verdict'] == "HP-DELTA WINS":
            wins += 1

    print(f"\n{'='*70}")
    total = len(results)
    if wins >= total // 2:
        print("DECISION: BUILD HP-DELTA")
        print(f"  Wins on {wins}/{total} datasets. Compressed depth < log2(m).")
        print("  The theorem is non-vacuous on actual data. Proceed.")
    else:
        print("DECISION: SKIP HP-DELTA, use counted B+-tree delta instead")
        print(f"  Only wins on {wins}/{total} datasets. Compressed depth >= log2(m).")
        print("  The B+-tree replacement gives better ROI for closing the 20x gap.")

    out = os.path.join(base, "results_q1", "trie_depth_measurement.json")
    with open(out, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved: {out}")


if __name__ == "__main__":
    main()
