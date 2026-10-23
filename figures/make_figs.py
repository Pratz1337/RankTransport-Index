#!/usr/bin/env python3
"""Regenerate every data figure of the paper (Figs. 2-6) from frozen result files.

Run from anywhere:  python3 figures/make_figs.py [REPO_ROOT]
REPO_ROOT defaults to the parent of this directory; inputs are read from REPO_ROOT/results/.
No number is typed in by hand: every plotted value is read from a JSON file
listed in SOURCES below.  Output: fig_scale.pdf, fig_specialists.pdf,
fig_rankcap.pdf, fig_range.pdf, fig_ledger.pdf (same directory as this script).
"""
import json, os, sys, statistics as st
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, ".."))
Q = os.path.join(R, "results")
SOURCES = {
    "eps": "verification_20260905/epsilon_corrected.json",
    "gate": "controlled_queries_20260905/analysis.json",
    "consol": "verification_20260905/signed_consolidation_200m.json",
    "spec": "adaptive_competitors_20260906/summary.json",
    # medians/min/max over 5 replicates, produced by summarize_replicates.py from repl{1..5}_*.json
    "repl": "replicated_rankcap/replicated_summary.json",
    "range_native": "benchmark_audit/range_stable_commoncrawl_host_real.json",
    "ledger_dir": "adaptive_ledger_20260906",
    "radix_hist": "early_prototype/prefix_radix_delta.json",
    "native_hist": "early_prototype/cpp_benchmark_results.json",
}
def load(key):
    with open(os.path.join(Q, SOURCES[key])) as f:
        return json.load(f)

# ---------------------------------------------------------------- style
TEXTW = 4.80  # paper text width is 122 mm
plt.rcParams.update({
    "text.usetex": True,                 # Computer Modern, Type 1, matches the paper
    "text.latex.preamble": r"\usepackage[T1]{fontenc}",
    "font.family": "serif",
    "font.size": 7.5, "axes.labelsize": 7.5, "axes.titlesize": 7.5,
    "xtick.labelsize": 6.8, "ytick.labelsize": 6.8, "legend.fontsize": 6.5,
    "pdf.fonttype": 42, "ps.fonttype": 42,
    "axes.linewidth": 0.6, "xtick.major.width": 0.5, "ytick.major.width": 0.5,
    "xtick.major.size": 2.5, "ytick.major.size": 2.5,
    "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": "#e3e3e3", "grid.linewidth": 0.5,
    "axes.axisbelow": True, "legend.frameon": False,
    "figure.dpi": 150, "savefig.bbox": "tight", "savefig.pad_inches": 0.02,
})
# Okabe-Ito based palette, one colour per system throughout the paper
C = {"H": "#D55E00", "Hbin": "#E69F00", "ART": "#0072B2", "HOT": "#56B4E9",
     "LITS": "#009E73", "CRadix": "#CC79A7", "PBDS": "#6E6E6E",
     "vec": "#8C8C8C", "set": "#C8C8C8", "btree": "#0072B2", "ink": "#222222"}

def panel(ax, s):
    ax.set_title(s, loc="left", pad=3)

def save(fig, name):
    fig.savefig(os.path.join(HERE, name))
    plt.close(fig)
    print("wrote", name)

# ---------------------------------------------------------------- Fig: 200M scale
def fig_scale():
    eps = load("eps")["result"]["rows"]
    gate = load("gate")
    con = load("consol")["result"]
    fig = plt.figure(figsize=(TEXTW, 2.25))
    gs = GridSpec(2, 3, figure=fig, height_ratios=[1.35, 0.75], hspace=1.2, wspace=0.75)

    # (a) segments vs epsilon, with certified max error annotated
    ax = fig.add_subplot(gs[0, 0])
    e = [r["epsilon"] for r in eps]; seg = [r["segments"] / 1e6 for r in eps]
    ax.plot(e, seg, "-o", color=C["H"], ms=3.5, lw=1.1)
    for x, y, r in zip(e, seg, eps):
        ok = r["bound_holds"] and r["repaired_segments"] == 0
        ax.annotate(r"%.2fM" % y, (x, y), textcoords="offset points",
                    xytext=(4, 2), fontsize=6)
    ax.set_xscale("log", base=2); ax.set_yscale("log")
    ax.set_xticks(e); ax.set_xticklabels([str(x) for x in e])
    ax.set_xlim(12, 300); ax.set_ylim(1.0, 25)
    ax.set_yticks([1.5, 3, 6, 12]); ax.set_yticklabels(["1.5", "3", "6", "12"])
    ax.yaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    ax.set_xlabel(r"requested bound $\epsilon$"); ax.set_ylabel("segments (millions)")
    assert all(r["certified_max_error"] == r["epsilon"] and r["bound_holds"] for r in eps)
    panel(ax, r"(a) certified segments")

    # (b) build + certify seconds
    ax = fig.add_subplot(gs[0, 1])
    xs = range(len(eps))
    b = [r["build_ms"] / 1e3 for r in eps]; c = [r["certify_ms"] / 1e3 for r in eps]
    ax.bar(xs, b, 0.62, color=C["H"], label="fit")
    ax.bar(xs, c, 0.62, bottom=b, color="#F2B38C", label="certify all keys")
    ax.set_xticks(list(xs)); ax.set_xticklabels([str(r["epsilon"]) for r in eps])
    ax.set_xlabel(r"requested bound $\epsilon$"); ax.set_ylabel("seconds")
    ax.set_ylim(0, 21)
    ax.legend(loc="upper center", ncol=2, handlelength=0.8, columnspacing=0.6, handletextpad=0.3, borderaxespad=0.0, labels=["fit", "certify"])
    panel(ax, r"(b) fit and audit time")

    # (c) paired binary/learned ratio per process
    ax = fig.add_subplot(gs[0, 2])
    order = [("uniform_membership_ns", "member"), ("balanced_membership_ns", "mix"),
             ("deleted_membership_ns", "deleted"), ("uniform_rank_ns", "rank"),
             ("range_ns", "count")]
    for i, (k, lab) in enumerate(order):
        m = gate["metrics"][k]; sp = m["paired_speedups"]
        jit = [-0.16, -0.08, 0, 0.08, 0.16]
        ax.scatter([i + j for j in jit], sp, s=7, color=C["H"], alpha=0.75, lw=0, zorder=3)
        ax.plot([i - 0.28, i + 0.28], [m["median_paired_speedup"]] * 2, color=C["ink"], lw=1.0, zorder=4)
    ax.axhline(1.0, color="#999999", lw=0.7, ls="--", zorder=2)
    ax.set_xticks(range(len(order))); ax.set_xticklabels([l for _, l in order], rotation=35, ha="right", fontsize=6.3)
    ax.set_ylim(0.9, 1.42); ax.set_ylabel(r"speed-up ($\times$)", labelpad=1)
    panel(ax, r"(c) learned speed-up")

    # (d) one generation lifecycle (single audit run), time axis in seconds
    ax = fig.add_subplot(gs[1, :])
    t = 0.0
    phases = [("initial build", con["initial_build_ms"], C["H"]),
              ("100k inserts", con["insert_ms"], "#E69F00"),
              ("100k deletes", con["base_delete_ms"], "#CC79A7"),
              ("export + flush live set", con["merge_write_flush_ms"], "#8C8C8C"),
              ("rebuild", con["rebuild_ms"], C["H"]),
              ("recertify", con["certify_ms"], "#F2B38C")]
    other = con["transition_total_ms"] - (con["merge_write_flush_ms"] + con["rebuild_ms"] + con["certify_ms"])
    phases.append(("checks", other, "#C8C8C8"))
    spans = []
    for lab, ms, col in phases:
        s = ms / 1e3
        ax.broken_barh([(t, s)], (0, 1), facecolors=col, edgecolor="white", lw=0.4)
        spans.append((lab, t, s)); t += s
    for lab, t0, s in spans:
        if s > 15:
            ax.text(t0 + s / 2, 0.5, lab, ha="center", va="center", fontsize=6.3,
                    color="white" if lab in ("initial build", "rebuild", "export + flush live set") else C["ink"])
    w0 = spans[1][1]
    ax.annotate(r"writes: %.2f\,s + %.2f\,s" % (con["insert_ms"] / 1e3, con["base_delete_ms"] / 1e3),
                xy=(w0 + 0.5, 1.0), xytext=(w0 + 4, 1.55), fontsize=6.3,
                arrowprops=dict(arrowstyle="-", lw=0.5, color=C["ink"]))
    ce = spans[5]
    ax.annotate("recertify %.1f\\,s" % ce[2], xy=(ce[1] + ce[2] / 2, 1.0), xytext=(ce[1] - 22, 1.55),
                fontsize=6.3, arrowprops=dict(arrowstyle="-", lw=0.5, color=C["ink"]))
    gen = spans[3][1]
    ax.annotate("", xy=(gen, -0.35), xytext=(t, -0.35),
                arrowprops=dict(arrowstyle="<->", lw=0.6, color=C["ink"]))
    ax.text((gen + t) / 2, -0.95, r"generation change %.1f\,s (service paused)" % (con["transition_total_ms"] / 1e3),
            ha="center", va="center", fontsize=6.3)
    ax.set_ylim(-1.3, 2.0); ax.set_xlim(0, t * 1.01); ax.set_yticks([])
    ax.spines["left"].set_visible(False); ax.grid(False)
    ax.set_xlabel("wall-clock seconds (one audit run, 199.9M keys)")
    panel(ax, r"(d) a full generation: writes are cheap, the rebuild is not")
    save(fig, "fig_scale.pdf")

# ---------------------------------------------------------------- Fig: specialists
def fig_specialists():
    d = load("spec")
    meths = [("hrtli", "HRT-LI", C["H"]), ("art", "ART", C["ART"]),
             ("hot", "HOT", C["HOT"]), ("lits", "LITS", C["LITS"])]
    fig = plt.figure(figsize=(TEXTW, 1.52))
    gs = GridSpec(1, 3, figure=fig, width_ratios=[2.2, 1, 1], wspace=0.6)
    ax = fig.add_subplot(gs[0, 0])
    wl = [("readonly", "read only"), ("uniform50", "uniform\nwrites"), ("prefix50", "burst\nwrites")]
    w = 0.19
    for j, (k, lab, col) in enumerate(meths):
        for i, (wk, _) in enumerate(wl):
            s = d["specialists"][wk][k]; x = i + (j - 1.5) * w
            ax.bar(x, s["median"], w * 0.92, color=col, label=lab if i == 0 else None)
            ax.plot([x, x], [s["minimum"], s["maximum"]], color=C["ink"], lw=0.6)
    ax.set_xticks(range(3)); ax.set_xticklabels([l for _, l in wl])
    ax.set_ylabel(r"s per $2^{20}$ operations")
    ax.legend(ncol=2, loc="upper left", handlelength=0.9, columnspacing=0.8, borderaxespad=0.0)
    ax.set_ylim(0, 6.6)
    panel(ax, "(a) membership time")
    for gi, (key, scale, ylab, title) in enumerate([
            ("construction", 1.0, "seconds", "(b) build"),
            ("whole_process_rss_kib", 1 / 1024, "MiB", "(c) peak RSS")]):
        ax = fig.add_subplot(gs[0, 1 + gi])
        for j, (k, lab, col) in enumerate(meths):
            s = d[key][k]
            ax.bar(j, s["median"] * scale, 0.7, color=col)
            ax.plot([j, j], [s["minimum"] * scale, s["maximum"] * scale], color=C["ink"], lw=0.6)
        ax.set_xticks(range(4)); ax.set_xticklabels([m[1] for m in meths], rotation=40, ha="right")
        ax.set_ylabel(ylab, labelpad=1); panel(ax, title)
    save(fig, "fig_specialists.pdf")

# ---------------------------------------------------------------- Fig: rank-capable comparison, n=5 replicates
PM = [("hrtli", "H", C["H"], None), ("hrtli_binary", "H-bin", C["Hbin"], None),
      ("counted_art", "C-ART", C["ART"], None), ("radix_ost", "C-Radix", C["CRadix"], None),
      ("pbds_rbtree", "PBDS", C["PBDS"], None)]
def repl_rows(corpus):
    """mode -> metric -> (median, min, max) over the replicates (summarize_replicates.py output)."""
    d = load("repl")["corpora"][corpus]
    out = {}
    for m, row in d.items():
        if m.startswith("_"):
            continue
        o = {}
        for k, v in row.items():
            key = {"build_seconds": "build", "bytes_per_key": "bpk"}.get(k, k.replace("ns_per_op:", ""))
            o[key] = (v["median"], v["min"], v["max"])
        out[m] = o
    return out

def fig_rankcap():
    cc, wk = repl_rows("cc_hosts_10m"), repl_rows("enwiki_titles")
    ops = [("writes_establish_delta", "writes"), ("member_hit", "member"),
           ("rank_live", "rank\nlive"), ("rank_absent_boundary", "rank\nabsent"),
           ("range_live_1000", "count\n1k span"), ("mixed_50w_rank_range", "mixed")]
    fig = plt.figure(figsize=(TEXTW, 1.95))
    gs = GridSpec(2, 3, figure=fig, width_ratios=[2.3, 0.05, 1], hspace=0.7, wspace=0.08)
    w = 0.16
    for row, (data, name) in enumerate([(cc, r"(a) Common Crawl hosts, 8M base"), (wk, r"(b) Wikipedia titles, 15.4M base")]):
        ax = fig.add_subplot(gs[row, 0])
        for j, (m, lab, col, _) in enumerate(PM):
            xs = [i + (j - 2) * w for i in range(len(ops))]
            md = [data[m][o][0] / 1e3 for o, _ in ops]
            lo = [(data[m][o][0] - data[m][o][1]) / 1e3 for o, _ in ops]
            hi = [(data[m][o][2] - data[m][o][0]) / 1e3 for o, _ in ops]
            ax.bar(xs, md, w * 0.92, color=col, label=lab)
            ax.errorbar(xs, md, yerr=[lo, hi], fmt="none", ecolor="#222222", elinewidth=0.5, capsize=0)
        ax.set_xticks(range(len(ops))); ax.set_xticklabels([l for _, l in ops], fontsize=6.3)
        ax.set_ylabel(r"$\mu$s per op"); ax.set_ylim(0, 12.5); ax.set_yticks([0, 4, 8])
        if row == 0:
            ax.legend(ncol=5, loc="upper left", handlelength=0.9, columnspacing=0.7, borderaxespad=0.0, bbox_to_anchor=(0, 1.04))
        panel(ax, name)
    ax = fig.add_subplot(gs[:, 2])
    for data, mk, fill in [(cc, "o", True), (wk, "s", False)]:
        for m, lab, col, _ in PM:
            if m == "hrtli_binary":
                continue
            b = data[m]["build"]
            ax.errorbar(data[m]["bpk"][0], b[0], yerr=[[b[0] - b[1]], [b[2] - b[0]]], fmt="none",
                        ecolor=col, elinewidth=0.7, capsize=1.5, zorder=2)
            ax.scatter(data[m]["bpk"][0], b[0], s=22, marker=mk,
                       facecolors=col if fill else "white", edgecolors=col, lw=1.0, zorder=3)
    for m, lab, col, _ in PM:
        if m == "hrtli_binary":
            continue
        ax.annotate(lab, (wk[m]["bpk"][0], wk[m]["build"][0]), textcoords="offset points",
                    xytext={"radix_ost": (-54, 3), "counted_art": (-30, -2), "pbds_rbtree": (-28, 2)}.get(m, (-4, 6)), fontsize=6.3, color=col)
    ax.set_xlabel("bytes per base key"); ax.set_ylabel("build seconds")
    ax.yaxis.set_label_position("right"); ax.yaxis.tick_right()
    ax.spines["right"].set_visible(True); ax.spines["left"].set_visible(False)
    ax.set_xlim(0, 160); ax.set_ylim(0, 21)
    ax.legend(handles=[Line2D([], [], marker="o", ls="", color="#555", ms=4, label="CC hosts"),
                       Line2D([], [], marker="s", ls="", mfc="white", mec="#555", ms=4, label="Wikipedia")],
              loc="upper left", bbox_to_anchor=(-0.03, 0.5), handletextpad=0.0, borderaxespad=0.0, fontsize=5.8)
    panel(ax, "(c) build vs footprint")
    save(fig, "fig_rankcap.pdf")

# ---------------------------------------------------------------- Fig: range count vs span
def fig_range():
    nat = load("range_native")
    cc, wk = repl_rows("cc_hosts_10m"), repl_rows("enwiki_titles")
    fig, axs = plt.subplots(1, 3, figsize=(TEXTW, 1.42), gridspec_kw=dict(width_ratios=[1.1, 1, 1], wspace=0.45))
    ax = axs[0]
    sels = sorted(nat["selectivities"], key=int)
    xs = [nat["selectivities"][s]["average_result_size"] for s in sels]
    for key, lab, col, mk in [("count_us_bootstrap95", "count", C["H"], "o"), ("scan_us_bootstrap95", "enumerate", C["vec"], "s")]:
        md = [nat["selectivities"][s][key]["median"] for s in sels]
        lo = [md[i] - nat["selectivities"][s][key]["low"] for i, s in enumerate(sels)]
        hi = [nat["selectivities"][s][key]["high"] - md[i] for i, s in enumerate(sels)]
        ax.errorbar(xs, md, yerr=[lo, hi], fmt="-" + mk, ms=3, lw=1.0, capsize=1.5, color=col, label=lab)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xticks([1, 10, 100, 1000]); ax.set_xticklabels(["1", "10", "100", "1k"])
    ax.set_yticks([1, 10, 100]); ax.set_yticklabels(["1", "10", "100"])
    ax.set_xlabel("keys in range"); ax.set_ylabel(r"$\mu$s per query")
    ax.legend(loc="upper left", handlelength=1.2, borderaxespad=0.1)
    panel(ax, "(a) earlier index, 9.04M")
    spans = [("range_live_10", 10), ("range_live_1000", 1000), ("range_live_100000", 100000)]
    for ax, data, title in [(axs[1], cc, "(b) CC hosts, $n=5$"), (axs[2], wk, "(c) Wikipedia, $n=5$")]:
        for m, lab, col, _ in PM:
            xs_ = [s for _, s in spans]
            ax.fill_between(xs_, [data[m][k][1] / 1e3 for k, _ in spans], [data[m][k][2] / 1e3 for k, _ in spans],
                            color=col, alpha=0.10, lw=0, zorder=1)
            ax.plot(xs_, [data[m][k][0] / 1e3 for k, _ in spans], "-o", ms=2.8,
                    lw=1.0 if m != "hrtli" else 1.6, color=col, label=lab, zorder=4 if m == "hrtli" else 3)
        ax.set_xscale("log"); ax.set_xticks([10, 1000, 100000]); ax.set_xticklabels(["10", "1k", "100k"])
        ax.set_ylim(0, 13.5); ax.set_yticks([0, 2, 4, 6, 8, 10]); ax.set_xlabel("approx. keys in range")
        panel(ax, title)
    axs[1].set_ylabel(r"$\mu$s per count")
    axs[1].legend(loc="upper left", ncol=2, fontsize=5.4, handlelength=1.0, columnspacing=0.5, handletextpad=0.3, borderaxespad=0.1)
    save(fig, "fig_range.pdf")

# ---------------------------------------------------------------- Fig: ledger and earlier fixtures
def fig_ledger():
    ldir = os.path.join(Q, SOURCES["ledger_dir"])
    var = [("linear", "linear", "#F2B38C"), ("adaptive", "fanout-aware", C["H"]), ("fenwick", "all Fenwick", "#8C3A0A")]
    led = {}
    for v, _, _ in var:
        rows = []
        for fn in sorted(os.listdir(ldir)):
            if fn.endswith("_%s.json" % v):
                with open(os.path.join(ldir, fn)) as f:
                    rows.append(json.load(f)["result"])
        led[v] = rows
    radix = load("radix_hist")["scenarios"]
    nat = load("native_hist")
    fig, axs = plt.subplots(1, 4, figsize=(TEXTW, 1.42), gridspec_kw=dict(width_ratios=[1.05, 0.95, 1, 1], wspace=0.95))
    # (a) current ledger variants: memory vs prefix time, every seed
    ax = axs[0]
    for v, lab, col in var:
        ax.scatter([r["estimated_ledger_bytes"] / 2**20 for r in led[v]], [r["prefix_seconds"] for r in led[v]],
                   s=9, color=col, label=lab, lw=0, alpha=0.9)
    ax.set_xlabel("ledger MiB"); ax.set_ylabel(r"s per $2^{20}$ prefix sums", labelpad=1)
    for v, lab, col in var:
        xm = sorted(r["estimated_ledger_bytes"] / 2**20 for r in led[v])[len(led[v]) // 2]
        ym = max(r["prefix_seconds"] for r in led[v])
        ax.text(xm, ym + 0.012, {"linear": "lin.", "adaptive": "fan.", "fenwick": "Fen."}[v], color=col,
                fontsize=5.8, ha="center", va="bottom")
    ax.set_ylim(1.2, 1.62); ax.set_xlim(24, 39)
    panel(ax, "(a) ledgers today")
    # (b) earlier radix ledger memory relative to a counted B+-tree, constructed fixtures
    ax = axs[1]
    names = {"dns": "DNS", "json": "JSON", "shared_hierarchy_100k": "shared", "shallow_100k": "shallow"}
    xs = list(range(len(radix)))
    ax.bar(xs, [r["memory_ratio"] for r in radix], 0.6, color=C["H"])
    ax.axhline(1.0, color=C["btree"], lw=1.0, ls="--")
    ax.text(len(radix) - 0.5, 1.03, r"B$^+$-tree", color=C["btree"], fontsize=5.8, ha="right", va="bottom")
    ax.set_xticks(xs); ax.set_xticklabels([names[r["name"]] for r in radix], rotation=40, ha="right")
    ax.set_ylabel("relative memory", labelpad=1); ax.set_ylim(0, 1.2); ax.set_yticks([0, 0.5, 1.0])
    panel(ax, "(b) earlier ledger")
    # (c,d) earlier compact native index vs ordered baselines on constructed fixtures
    idx = [("HRT-LI native rank transport", "earlier HRT-LI", C["H"]),
           ("sorted vector lower_bound", "sorted vector", C["vec"]), ("std::set", "std::set", C["set"])]
    dss = [("filesystem", "files"), ("dns", "DNS"), ("json", "JSON")]
    w = 0.26
    for ax, key, ylab, title in [(axs[2], "avg_lookup_latency_ns", "ns per lookup", "(c) lookups"),
                                 (axs[3], "insert_time_ms", "ms per bulk insert", "(d) inserts")]:
        for j, (iname, lab, col) in enumerate(idx):
            for i, (ds, _) in enumerate(dss):
                r = next(x for x in nat[ds] if x["index_name"] == iname)
                x = i + (j - 1) * w
                ax.bar(x, r[key + "_median"], w * 0.92, color=col, label=lab if i == 0 else None)
                ax.plot([x, x], [r[key + "_bootstrap95_low"], r[key + "_bootstrap95_high"]], color=C["ink"], lw=0.6)
        ax.set_xticks(range(3)); ax.set_xticklabels([l for _, l in dss], rotation=40, ha="right")
        ax.set_ylabel(ylab, labelpad=1); panel(ax, title)
    axs[2].set_ylim(0, 260); axs[3].set_ylim(0, 1.6)
    axs[3].legend(loc="upper left", handlelength=0.8, handletextpad=0.3, borderaxespad=0.0, fontsize=5.8, labelspacing=0.2)
    save(fig, "fig_ledger.pdf")

# ---------------------------------------------------------------- main
if __name__ == "__main__":
    print("repository root:", R)
    fig_scale(); fig_specialists(); fig_rankcap(); fig_range(); fig_ledger()
