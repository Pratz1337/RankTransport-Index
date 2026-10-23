"""Replicated rank-capable (equal-contract) sessions.

Runs N independent Modal containers per corpus, each a fresh machine, with the
method order rotated per replicate so no method always runs first. Same trace
(seed) as the pilot sessions, so results are directly comparable. Records are
written to results/replicated_rankcap/.

Usage (from the repository root):
    python -m modal run scripts/modal/modal_replicate.py::replicate --n 5
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # modal_hrtli.py lives alongside
from modal_hrtli import app, equal_contract_session, save_record  # noqa: F401  (app needed by modal run)

PARAMS = {"cc_hosts_10m": 80_000, "enwiki_titles": 150_000}
MODES = ["hrtli", "hrtli_binary", "counted_art", "radix_ost", "pbds_rbtree"]


@app.local_entrypoint()
def replicate(n: int = 5, seed: int = 20261023, epsilon: int = 64, queries: int = 1_048_576,
              label: str = "repl") -> None:
    jobs = []
    for corpus, delta in PARAMS.items():
        for r in range(n):
            order = MODES[r % len(MODES):] + MODES[:r % len(MODES)]
            jobs.append((corpus, delta, queries, seed, epsilon, order, None, "standard"))
    for (corpus, delta, *_), (r, record) in zip(jobs, enumerate(equal_contract_session.starmap(jobs, order_outputs=True))):
        rep = r % n
        record["label"] = f"{label}{rep + 1}"
        record["replicate"] = rep + 1
        record["replicates_total"] = n
        save_record(f"{label}{rep + 1}_{corpus}_d{delta}_s{seed}_e{epsilon}.json", record)
