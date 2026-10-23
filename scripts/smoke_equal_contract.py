"""Build hrtli_cpp/benchmark_equal_contract.cpp and run every rank-capable method on a
small synthetic key set. Checks only that each method builds, runs and passes the
driver's own answer checks; the timings are meaningless and are not reported."""
from __future__ import annotations

import json
import random
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c++17", "-O3", "-DNDEBUG", "-march=x86-64-v3", "-DHRTLI_WITH_COUNTED_ART", "-DHRTLI_WITH_FENCED"]
MODES = ["hrtli", "hrtli_binary", "counted_art", "radix_ost", "pbds_rbtree"]


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="hrtli_equal_contract_smoke_") as tmp:
        work = Path(tmp)
        binary = work / "equal_contract"
        subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "g++", *FLAGS, f"-I{ROOT / 'hrtli_cpp'}",
                        str(ROOT / "hrtli_cpp/benchmark_equal_contract.cpp"), "-o", str(binary)], check=True)
        rng = random.Random(7)
        words = ["com", "org", "net", "de", "uk", "io", "example", "alpha", "beta", "www", "mail", "cdn",
                 "shop", "blog", "news", "api", "static", "img"]
        keys: set[str] = set()
        while len(keys) < 300_000:  # range streams need more than 100,000 live keys
            keys.add(".".join(rng.choice(words) for _ in range(rng.randint(2, 5))) + str(rng.randint(0, 99_999)))
        ordered = sorted(keys)
        base = [k for i, k in enumerate(ordered) if i % 5 != 4]
        arrivals = [k for i, k in enumerate(ordered) if i % 5 == 4]
        (work / "base.txt").write_text("\n".join(base) + "\n")
        (work / "arrivals.nul").write_bytes(b"\0".join(k.encode() for k in arrivals) + b"\0")
        failed = 0
        for mode in MODES:
            proc = subprocess.run([str(binary), mode, str(work / "base.txt"), str(work / "arrivals.nul"),
                                   str(len(base)), str(len(arrivals)), "20000", "65536", "20261023", "64"],
                                  capture_output=True, text=True)
            ok = proc.returncode == 0 and all(s.get("answers_checked") for s in json.loads(proc.stdout)["streams"])
            failed += not ok
            print(("PASS " if ok else "FAIL ") + mode + ("" if ok else ": " + proc.stderr[-300:]))
        return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
