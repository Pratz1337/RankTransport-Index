# External Baseline Lockfile

`benchmark_external.cpp` is intended to be compiled only against these pinned
competitor revisions. Do not claim external-baseline numbers from floating
branches.

| Baseline | Repository | Commit | License note |
|---|---|---|---|
| ALEX | https://github.com/microsoft/ALEX.git | `4370da6aa8b509fdc9b0d2c49faa0624b0078589` | MIT |
| PGM-index | https://github.com/gvinciguerra/PGM-index.git | `c6fcf3d34e55eb0061b01e2f49dfcbdb711f1407` | Apache-2.0 |
| LIPP | https://github.com/Jiacheng-WU/LIPP.git | `fe6ca4954f00875482f9e4dd63b34dae2384d23b` | MIT |
| libart / ART | https://github.com/armon/libart.git | `301046804af165269e37da6725f5a4aec9ecc881` | BSD-3-Clause-style license text in repository |
| HOT | https://github.com/speedskater/hot.git | `96bf6fb7103b27e50e16a6026db8974c090ee84a` | ISC |
| LITS | https://github.com/schencoding/lits.git | `c9026ac9645b4af1f6e3cb27b42351220f4376d4` | No root `LICENSE` file found at the pinned revision; keep as source-only optional baseline until redistribution permission is confirmed |

Use `scripts/setup_external_baselines.sh` to reproduce the checkout layout.
