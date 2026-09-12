# Frozen current-default specialist comparison

Protocol specified before the first performance run, 6 September 2026.

- Same natural 10,000,000-key selection and split as the retained preparation:
  8,000,000 base keys, 2,000,000 eligible arrivals. Every twentieth key across
  the verified 200M source prefix is sampled. This is not full-release random sampling.
- Current packed HRT-LI: segment-fence recovery, guarded mutation probes and
  Fenwick sibling masses, epsilon 64, no fingerprint table. Sources are frozen
  under `source_measured/`; no result is back-attributed to an earlier build.
- ART, upstream HOT and patched LITS implement membership/insert/delete set
  semantics. No rank operation is substituted for their missing rank adapters.
  ART includes the terminal NUL in its byte length; HOT borrows stable keys.
  The complete input must fit HOT's 254-byte domain or the session is rejected.
- LITS and upstream HOT compile into separate binaries from the same driver
  because LITS vendors conflicting HOT namespaces/header guards. Dependency
  files and hashes verify separation. Pinned upstream revisions and patches
  accompany binary hashes. ART is unmodified. LITS's reported deallocation
  leaks remain unfixed; this is finite-trace behavior, not steady-state memory.
- Four seeds, 20260906 through 20260909; fresh process/reset for every cell.
  Four-method order rotates by seed to balance first/last position. All
  completed cells, including unfavorable results, are retained.
- 1,048,576 operations per cell. Workloads: read-only; uniform 50% successful
  writes; and 50% writes in 64-operation bursts under the natural `com.` prefix,
  alternating with 64 full-pool reads. Successful deletions and insertions
  alternate, keeping live cardinality within one key. Four seeds x three
  workloads x four methods = 48 planned process records.
- Independent eligibility pools generate the trace before an index exists;
  they are freed before build. A separate bitmap computes every expected
  answer. Check all initial/final memberships, each timed answer and cross-method
  trace hashes. Correctness checks are outside the timed workload.
- Same optimized C++17 flags, CPU 2, one process at a time. No compiler or PDF
  rendering runs concurrently. Stop guest background services; retain sampled
  Windows memory and guest swap counters; shut WSL down after completion/failure.
- Report median and observed min/max across the four process/seed observations.
  No confidence interval or cross-machine generalization is inferred. RSS is
  whole-process peak, including common input/trace/oracle/build overhead, not
  index-only memory. Input preparation and hashing are outside the timer.
- A larger all-method run is not presumed feasible: this host has 16 GiB RAM
  and a 12 GiB WSL limit. Assess measured memory first. This protocol does not
  close the 200M external-comparison or replicated maintenance gaps.
