# Common Crawl inputs and the 200M query experiment

## Source and exact selection

The input is the Common Crawl host-level web graph release
`cc-main-2026-may-jun-jul`. The retained
[metadata](../tmp/commoncrawl-2026-may-jun-jul-local/prepared_interleaved/commoncrawl_hosts_200m_metadata.json)
identifies its official announcement, all 48 compressed vertex-shard URLs and
SHA-256 digests, record counts, output sizes and prepared-file digests.
Download those shards from the recorded `data.commoncrawl.org` URLs and verify
each compressed file before preparation. The raw corpus is not hosted here.

The preparation code verifies 240,380,987 contiguous host IDs and strictly
ascending raw UTF-8 key order across the release. It selects IDs 0 through
199,999,999, holding out every 2000th selected key: 199,900,000 base keys and
100,000 insertion keys. This is a contiguous selection, not a random sample.

On a suitably provisioned machine, from the repository root:

```sh
python scripts/prepare_commoncrawl_200m.py --shard-dir PATH_TO_48_SHARDS --output-dir NEW_PREPARED_DIRECTORY
```

Use a new output directory: this preparation program writes its output files.
Compare the generated base/insert digests and counts with the retained metadata.
The source format is `<host ID, reverse-domain host>`; use the host string, not
the numerical ID, as the index key.

## Rebuild the measured query path, not the later default

The frozen sources for trials 2 through 6 are in
`results_q1/controlled_queries_20260905/source/`. In particular, these trials
predate the segment-fence and Fenwick refinements. The paired paths share the
same index state, use no fingerprint table, and perform an internal recovery
ablation. They do not compare HRT-LI against another index.

Example build and single-process invocation on Linux, with GCC/G++ and `taskset`:

```sh
mkdir -p build
g++ -std=c++17 -O3 -DNDEBUG -march=native results_q1/controlled_queries_20260905/source/benchmark_packed_query_paths.cpp -o build/query_paths
taskset -c 2 build/query_paths NEW_PREPARED_DIRECTORY/commoncrawl_hosts_200m_initial.txt NEW_PREPARED_DIRECTORY/commoncrawl_hosts_200m_insert.txt 199900000 2000 64 1000000 4 learned_binary
```

Choose an available CPU if logical CPU 2 is not present, and record the change.
The saved trials contain the exact arguments, timestamps, binary/source hashes,
oracle results and timings. Use `scripts/record_experiment.py --help` to capture
new invocations into a new output directory with their own identities. Never
overwrite or relabel the deposited measurements as new runs.

The original shell/PowerShell launchers retain the experiment laptop's absolute
paths, monitoring setup and frozen binary location. They document execution,
not a portable installer: adapt paths and create a fresh build/output location
before using them. Rebuilding can change binary hashes even for identical source.

## Scope of the retained measurements

Five fresh processes each run four paired rounds. The records report 164M timed
answers checked against the driver's independent oracle, including surviving,
inserted and deleted-key cases and ordered queries. The relevant summary is
`results_q1/reviewer_response_20260906/query_descriptive.json`, recomputed by
`scripts/summarize_query_runs_descriptively.py`.

Keep all five runs. Three have sampled host page-out activity. These are
descriptive observations on one shared Windows/WSL2 laptop, one dataset selection
and one seed, not confidence intervals over machines or datasets.

The error-budget sweep and the single offline-consolidation timing are separate
records under `results_q1/reassessment_20260905`, with their own frozen source
directories. Consult `docs/BUILD_PROVENANCE.json` rather than assuming they use
the query binary. The consolidation observation does not establish atomic,
online or crash-safe maintenance.

For the smaller natural-key specialist sample and baseline dependencies, use
[CURRENT_COMPARISON_REPRODUCTION.md](CURRENT_COMPARISON_REPRODUCTION.md).
