"""Regression fixtures for evidence gates; generated keys are not paper metrics."""
import unittest
from unittest.mock import patch

import benchmark_hrtli as benchmark
from hli.rank_transport import RankTransportIndex


class BenchmarkAuditTests(unittest.TestCase):
    def test_checks_insertions_beyond_old_64_key_sample(self):
        index = RankTransportIndex(["/base"], 0)
        inserted = [f"/new/{i:03d}" for i in range(100)]
        for key in inserted:
            index.insert(key)
        expected = {"/base", *inserted}
        result = benchmark.audit_live_state(index, {"/base": 0}, expected, 0)
        self.assertEqual(result["checked_inserted_ranks"], 100)
        real_rank = index.exact_rank
        real_lookup = index.lookup
        with patch.object(index, "exact_rank", side_effect=lambda key: real_rank(key) + (key == inserted[80])), \
             patch.object(index, "lookup", side_effect=lambda key, pred: real_lookup(key, pred) + (key == inserted[80])):
            with self.assertRaisesRegex(ValueError, "independent oracle"):
                benchmark.audit_live_state(index, {"/base": 0}, expected, 0)

    def test_oracle_does_not_read_index_snapshot(self):
        index = RankTransportIndex(["/a", "/b", "/c"], 0)
        index.delete("/b")
        index.insert("/ab")
        with patch.object(index, "snapshot_keys", side_effect=RuntimeError("not an oracle")):
            result = benchmark.audit_live_state(index, {"/a": 0, "/b": 1, "/c": 2}, {"/a", "/ab", "/c"}, 0)
        self.assertEqual(result["checked_surviving_base"], 2)
        self.assertEqual(result["transported_max_error"], 0)

    def test_rejects_unrequested_error_instead_of_widening_certificate(self):
        with patch.object(benchmark, "PiecewiseLinearIndex") as model:
            model.return_value.predict_position.return_value = 65
            model.return_value.segments = []
            model.return_value.memory_bytes.return_value = 0
            with self.assertRaisesRegex(ValueError, "Requested epsilon 64 failed: realized 65"):
                benchmark.build_certified_base_model(["/a"], 64)

    def test_rejects_wrong_live_size(self):
        index = RankTransportIndex(["/a"], 0)
        with self.assertRaisesRegex(ValueError, "cardinality"):
            benchmark.audit_live_state(index, {"/a": 0}, {"/a", "/b"}, 0)

    def test_rejects_transport_error_even_when_exact_ranks_agree(self):
        index = RankTransportIndex(["/a"], 0)
        with patch.object(index, "transport_base_prediction", return_value=1):
            with self.assertRaisesRegex(ValueError, "Transported error"):
                benchmark.audit_live_state(index, {"/a": 0}, {"/a"}, 0)

    def test_reports_actual_prefix_closed_base_size(self):
        with patch.object(benchmark, "benchmark_update_baselines", return_value={}):
            result = benchmark.run_rank_transport_experiment("regression only", ["/a/b", "/c/d"])
        self.assertEqual(result["dataset_stats"]["num_keys"], 2)
        self.assertEqual(result["indexed_base_stats"]["num_keys"], 5)
        self.assertEqual(result["history"][-1]["checked_inserted_ranks"], 630)
        self.assertIn("not natural", result["mutation_source"])

    def test_update_comparison_constructs_fresh_hrt_state(self):
        with patch.object(benchmark, "RankTransportIndex", wraps=RankTransportIndex) as constructor:
            benchmark.benchmark_update_baselines(["/a", "/c"], ["/b"])
        constructor.assert_called_once_with(["/a", "/c"], 0)


if __name__ == "__main__":
    unittest.main()
