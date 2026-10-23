#!/usr/bin/env python3
"""Print Table 1 (rank-capable comparison, median [min, max] over five containers).

Input : results/replicated_rankcap/replicated_summary.json (from summarize_replicates.py)
Output: LaTeX rows on stdout (or write to a file with: python3 figures/make_table1.py > table1.tex)
Times are microseconds per operation (ns/op / 1000); the count column is the
range_live_1000 stream (ranges of about 1,000 live keys); build is in seconds.
Usage: python3 figures/make_table1.py [REPO_ROOT]
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, ".."))
S = json.load(open(os.path.join(R, "results", "replicated_rankcap", "replicated_summary.json")))

METHODS = [("hrtli", "H"), ("hrtli_binary", "H-bin"), ("counted_art", "C-ART"),
           ("radix_ost", "C-Radix"), ("pbds_rbtree", "PBDS")]
CORPORA = [("cc_hosts_10m", "Hosts"), ("enwiki_titles", "Titles")]
STREAMS = ["rank_live", "range_live_1000", "mixed_50w_rank_range"]


def cell(v, scale=1.0):
    return f"{v['median'] / scale:.2f} {{\\tiny[{v['min'] / scale:.2f}, {v['max'] / scale:.2f}]}}"


print(r"\toprule")
print(r" & & Build (s) & Bytes/key & Rank, live & Count, 1k & Mixed\\")
for ci, (corpus, cname) in enumerate(CORPORA):
    print(r"\midrule")
    rows = S["corpora"][corpus]
    for mi, (m, label) in enumerate(METHODS):
        r = rows[m]
        bpk = r["bytes_per_key"]
        assert bpk["min"] == bpk["max"], f"{corpus}/{m}: bytes per key differ across replicates"
        cols = [cell(r["build_seconds"]), f"{bpk['median']:.1f}"]
        cols += [cell(r["ns_per_op:" + s], 1e3) for s in STREAMS]
        print(f"{cname if mi == 0 else ''} & {label} & " + " & ".join(cols) + r"\\")
print(r"\bottomrule")
