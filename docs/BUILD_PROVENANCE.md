# Consolidated experiment/build provenance

Run times are UTC, taken from experiment records; compilation dates are not recorded.
SHA-256 prefixes below are display identifiers; full digests and source maps are in
`BUILD_PROVENANCE.json`. No clean commit is asserted for a dirty working-tree run.

| Experiment | Run UTC | Binary SHA-256 (prefix) | Interpretation |
|---|---|---|---|
| 200M paired queries, process 2 | 2026-09-05T00:36:59.089365+00:00 | `7e24e812e0522c5b` | Internal recovery ablation and inverse writes; pre-segment-fence and pre-Fenwick |
| 200M paired queries, process 3 | 2026-09-05T00:43:22.424022+00:00 | `7e24e812e0522c5b` | Internal recovery ablation and inverse writes; pre-segment-fence and pre-Fenwick |
| 200M paired queries, process 4 | 2026-09-05T00:47:25.651699+00:00 | `7e24e812e0522c5b` | Internal recovery ablation and inverse writes; pre-segment-fence and pre-Fenwick |
| 200M paired queries, process 5 | 2026-09-05T00:51:27.187083+00:00 | `7e24e812e0522c5b` | Internal recovery ablation and inverse writes; pre-segment-fence and pre-Fenwick |
| 200M paired queries, process 6 | 2026-09-05T00:55:33.644334+00:00 | `7e24e812e0522c5b` | Internal recovery ablation and inverse writes; pre-segment-fence and pre-Fenwick |
| 199.9M epsilon sweep | 2026-09-04T23:10:38.106137+00:00 | `41c5378e8ca2efb7` | Exhaustive epsilon table; earlier certified-builder snapshot |
| 199.9M signed consolidation | 2026-09-04T23:34:15.424097+00:00 | `486bfc4df5856b36` | Lifecycle table offline transition; earlier allocation-safety snapshot |
| 1000000 natural-key pilot, lits | 2026-09-05T17:29:34.107312+00:00 | `b0e7777d116b466a` | One pilot pair/scale; 10M unfavorable result disclosed in competitive-baseline section; not replicated |
| 1000000 natural-key pilot, hrtli | 2026-09-05T17:29:35.851146+00:00 | `b0e7777d116b466a` | One pilot pair/scale; 10M unfavorable result disclosed in competitive-baseline section; not replicated |
| 10000000 natural-key pilot, lits | 2026-09-05T17:29:40.638233+00:00 | `b0e7777d116b466a` | One pilot pair/scale; 10M unfavorable result disclosed in competitive-baseline section; not replicated |
| 10000000 natural-key pilot, hrtli | 2026-09-05T17:29:47.966399+00:00 | `b0e7777d116b466a` | One pilot pair/scale; 10M unfavorable result disclosed in competitive-baseline section; not replicated |
| 10M mutation-path pair 1, binary_mutation | 2026-09-05T17:36:01.712111+00:00 | `b0e7777d116b466a` | Development-only paired refinement; not the 200M experiment |
| 10M mutation-path pair 1, learned_mutation | 2026-09-05T17:36:12.656257+00:00 | `ebb56d47eeeaeaed` | Development-only paired refinement; not the 200M experiment |
| 10M mutation-path pair 2, binary_mutation | 2026-09-05T17:36:33.285416+00:00 | `b0e7777d116b466a` | Development-only paired refinement; not the 200M experiment |
| 10M mutation-path pair 2, learned_mutation | 2026-09-05T17:36:23.168298+00:00 | `ebb56d47eeeaeaed` | Development-only paired refinement; not the 200M experiment |
| 10M mutation-path pair 3, binary_mutation | 2026-09-05T17:36:44.538256+00:00 | `b0e7777d116b466a` | Development-only paired refinement; not the 200M experiment |
| 10M mutation-path pair 3, learned_mutation | 2026-09-05T17:36:55.350009+00:00 | `ebb56d47eeeaeaed` | Development-only paired refinement; not the 200M experiment |
| All-node prototype, readonly, seed 20260906, hrtli | 2026-09-06T17:58:45.020177+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260906, art | 2026-09-06T17:58:54.564966+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260906, hot | 2026-09-06T17:59:00.853861+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260906, lits | 2026-09-06T17:59:07.299395+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260907, hrtli | 2026-09-06T17:59:35.244635+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260907, art | 2026-09-06T17:59:14.621310+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260907, hot | 2026-09-06T17:59:21.011925+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260907, lits | 2026-09-06T17:59:27.695593+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260908, hrtli | 2026-09-06T17:59:58.012897+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260908, art | 2026-09-06T18:00:07.172650+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260908, hot | 2026-09-06T17:59:44.772847+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260908, lits | 2026-09-06T17:59:50.915047+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260909, hrtli | 2026-09-06T18:00:20.340458+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260909, art | 2026-09-06T18:00:29.547465+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260909, hot | 2026-09-06T18:00:35.798327+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, readonly, seed 20260909, lits | 2026-09-06T18:00:13.257669+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260906, hrtli | 2026-09-06T18:00:42.189736+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260906, art | 2026-09-06T18:00:55.602901+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260906, hot | 2026-09-06T18:01:02.673918+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260906, lits | 2026-09-06T18:01:09.951523+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260907, hrtli | 2026-09-06T18:01:41.589716+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260907, art | 2026-09-06T18:01:18.181944+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260907, hot | 2026-09-06T18:01:25.811824+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260907, lits | 2026-09-06T18:01:33.222615+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260908, hrtli | 2026-09-06T18:02:10.331564+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260908, art | 2026-09-06T18:02:23.698505+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260908, hot | 2026-09-06T18:01:54.733907+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260908, lits | 2026-09-06T18:02:02.009845+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260909, hrtli | 2026-09-06T18:02:39.142999+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260909, art | 2026-09-06T18:02:52.713218+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260909, hot | 2026-09-06T18:02:59.698508+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, uniform50, seed 20260909, lits | 2026-09-06T18:02:30.747629+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260906, hrtli | 2026-09-06T18:03:07.238316+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260906, art | 2026-09-06T18:03:19.849091+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260906, hot | 2026-09-06T18:03:26.909527+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260906, lits | 2026-09-06T18:03:34.259534+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260907, hrtli | 2026-09-06T18:04:05.505816+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260907, art | 2026-09-06T18:03:42.355285+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260907, hot | 2026-09-06T18:03:49.658049+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260907, lits | 2026-09-06T18:03:57.364470+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260908, hrtli | 2026-09-06T18:04:33.722315+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260908, art | 2026-09-06T18:04:46.726571+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260908, hot | 2026-09-06T18:04:18.264194+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260908, lits | 2026-09-06T18:04:25.493571+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260909, hrtli | 2026-09-06T18:05:01.889222+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260909, art | 2026-09-06T18:05:14.501406+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260909, hot | 2026-09-06T18:05:21.762964+00:00 | `a7cf85f438722af0` | Development-only prototype; not pooled with current table |
| All-node prototype, prefix50, seed 20260909, lits | 2026-09-06T18:04:53.697463+00:00 | `f2c27d9eda34ea54` | Development-only prototype; not pooled with current table |
| Current fanout-aware, readonly, seed 20260906, hrtli | 2026-09-07T06:09:39.329680+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260906, art | 2026-09-07T06:09:49.484850+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260906, hot | 2026-09-07T06:09:55.125262+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260906, lits | 2026-09-07T06:10:00.919565+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260907, hrtli | 2026-09-07T06:10:25.488562+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260907, art | 2026-09-07T06:10:07.359265+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260907, hot | 2026-09-07T06:10:13.551713+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260907, lits | 2026-09-07T06:10:19.097374+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260908, hrtli | 2026-09-07T06:10:45.847097+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260908, art | 2026-09-07T06:10:54.035323+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260908, hot | 2026-09-07T06:10:33.719233+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260908, lits | 2026-09-07T06:10:39.469466+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260909, hrtli | 2026-09-07T06:11:07.374154+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260909, art | 2026-09-07T06:11:16.054861+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260909, hot | 2026-09-07T06:11:22.072213+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, readonly, seed 20260909, lits | 2026-09-07T06:11:00.192601+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260906, hrtli | 2026-09-07T06:11:27.965126+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260906, art | 2026-09-07T06:11:39.534257+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260906, hot | 2026-09-07T06:11:46.004148+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260906, lits | 2026-09-07T06:11:54.132599+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260907, hrtli | 2026-09-07T06:12:27.115579+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260907, art | 2026-09-07T06:12:02.909078+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260907, hot | 2026-09-07T06:12:11.423510+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260907, lits | 2026-09-07T06:12:18.927527+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260908, hrtli | 2026-09-07T06:12:52.377214+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260908, art | 2026-09-07T06:13:04.334339+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260908, hot | 2026-09-07T06:12:38.685987+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260908, lits | 2026-09-07T06:12:45.016848+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260909, hrtli | 2026-09-07T06:13:20.467721+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260909, art | 2026-09-07T06:13:34.724839+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260909, hot | 2026-09-07T06:13:41.398315+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, uniform50, seed 20260909, lits | 2026-09-07T06:13:11.205913+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260906, hrtli | 2026-09-07T06:13:48.216392+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260906, art | 2026-09-07T06:14:00.779510+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260906, hot | 2026-09-07T06:14:08.971856+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260906, lits | 2026-09-07T06:14:16.149716+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260907, hrtli | 2026-09-07T06:14:48.078233+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260907, art | 2026-09-07T06:14:24.371253+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260907, hot | 2026-09-07T06:14:31.657534+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260907, lits | 2026-09-07T06:14:39.283723+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260908, hrtli | 2026-09-07T06:15:14.537811+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260908, art | 2026-09-07T06:15:26.656192+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260908, hot | 2026-09-07T06:14:59.659504+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260908, lits | 2026-09-07T06:15:06.648131+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260909, hrtli | 2026-09-07T06:15:41.106121+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260909, art | 2026-09-07T06:15:52.825886+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260909, hot | 2026-09-07T06:15:59.731029+00:00 | `9e10f0e420612d9b` | 10M common natural pool; current main table |
| Current fanout-aware, prefix50, seed 20260909, lits | 2026-09-07T06:15:33.390211+00:00 | `4f91c744c78b4d92` | 10M common natural pool; current main table |
| Direct ledger, seed 20260906, linear | 2026-09-06T18:13:38.674187+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260906, fenwick | 2026-09-06T18:13:41.447646+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260906, adaptive | 2026-09-06T18:13:44.379878+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260907, linear | 2026-09-06T18:13:52.403115+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260907, fenwick | 2026-09-06T18:13:47.066315+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260907, adaptive | 2026-09-06T18:13:49.833312+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260908, linear | 2026-09-06T18:13:57.458018+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260908, fenwick | 2026-09-06T18:13:59.980638+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260908, adaptive | 2026-09-06T18:13:54.928303+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260909, linear | 2026-09-06T18:14:02.709901+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260909, fenwick | 2026-09-06T18:14:05.358919+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260909, adaptive | 2026-09-06T18:14:08.135085+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260910, linear | 2026-09-06T18:14:16.435681+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260910, fenwick | 2026-09-06T18:14:10.696137+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260910, adaptive | 2026-09-06T18:14:13.493492+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260911, linear | 2026-09-06T18:14:21.753511+00:00 | `d10d746d913cd1e5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260911, fenwick | 2026-09-06T18:14:24.321442+00:00 | `36b9627b2bcbed9b` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Direct ledger, seed 20260911, adaptive | 2026-09-06T18:14:19.157752+00:00 | `2d6016237efe9ef5` | Main ledger trade-off table; same natural signed-key workload, separate binaries |
| Compact certificate fixtures | Unavailable | `Unavailable` | Supplementary history only; internal oracle; removed from main figures/tables |
| Compact native comparisons | Unavailable | `Unavailable` | Compact native comparison and update/memory figures; earlier implementation |
| Radix / counted B+-tree ledger | Unavailable | `Unavailable` | Historical ledger comparison; predates Fenwick revision |
| 9.04M range-count / scan | Unavailable | `Unavailable` | Range-count/scan figure; earlier native index |
| 9.04M ART/HOT comparison | Unavailable | `Unavailable` | Historical paragraph and supplementary ranges; not current packed API |

## Identity limits

The five 200M query processes share the same recorded binary. The epsilon and
consolidation binaries are different builds and must not be silently combined
into one current-version performance claim. Segment-fence and learned-mutation
refinements came later. Their developmental records are listed separately.

For the historical compact, range, ledger and ART/HOT result files, the inspected
records do not provide experiment-time binary/source hashes or reliable run dates.
Their result-file hashes are retained in the JSON, but are not binary identities.
File modification times and a broad dirty-tree manifest are not substituted for
missing experiment provenance. These rows remain explicitly limited historical
context; rerunning the current default is the preferred way to close the gap.

Regenerate from workspace records: `python scripts/build_provenance_table.py`.
The supplement carries identifiers for development records; not every external
baseline dependency is redistributed in the manuscript source ZIP.
