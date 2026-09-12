# Current adaptive-ledger specialist comparison

Specified before timing this revision, 6 September 2026. Retains the full
48-cell protocol in `results_q1/current_competitors_20260906/PROTOCOL.md`:
the same verified 10M natural sample, 8M/2M split, 1,048,576 operations,
four seeds (20260906-20260909), four separately reset methods, and read-only,
uniform 50% writes, and `com.`-prefix 64-write/64-read bursts. All answers
and every initial/final membership are checked; method order rotates by seed.
Input files, patched upstream sources, operation traces and compiler flags
remain the same. All four methods are rerun; earlier competitor timings are
not spliced into a later HRT-LI run.

The sole main-index change is fanout-aware mass storage: nodes with at most
16 children have no allocated Fenwick array; wider nodes own one. Promotion
prepares a valid cache before child insertion; demotion releases the cache.
The threshold is an engineering choice, not a tuned optimal value. This
revision responds to an observed regression in the all-node Fenwick prototype;
the old measurements remain under their own identities. Reusing the workload
is development validation, not a new held-out dataset.

Retain descriptive medians/ranges, finite whole-process RSS, every result,
source/binary/dependency hashes, and sampled host/guest resource counters.
The baseline safety patches and unresolved LITS deallocation leaks are
unchanged. This run does not assert 200M competitive performance, full-release
random sampling, index-only memory, online maintenance or concurrency.
