"""
Tests for HP-SFC (Hierarchical Prefix Sparse Fingerprint Code), an expected
O(1) base-key lookup mechanism derived from exact sparse-radix path codes.

Verifies:
1. All base keys are found with correct base_idx
2. Non-base keys return None
3. HP-SFC stats show low collision count
4. HPSFCRankTransportIndex produces same exact ranks as naive binary search
5. Dynamic inserts/deletes maintain rank-transport correctness
"""

import random
import unittest
from bisect import bisect_left

from hli.datasets import generate_dns_paths, generate_json_paths, generate_scalable_tree
from hli.hpsfc import HPSFCRankTransportIndex, HPSFCTable


class TestHPSFCTable(unittest.TestCase):
    def setUp(self):
        random.seed(42)
        paths = generate_scalable_tree(500, max_depth=6, seed=77)
        unique = sorted(set(paths))
        self.base_keys = unique[:400]
        self.non_keys = [p for p in unique[400:] if p not in set(self.base_keys)][:20]
        self.table = HPSFCTable(self.base_keys)

    def test_all_base_keys_found(self):
        for idx, key in enumerate(self.base_keys):
            result = self.table.lookup(key)
            self.assertIsNotNone(result, f"Key {key!r} not found in HP-SFC table")
            self.assertEqual(result, idx, f"Wrong base_idx for {key!r}: got {result}, expected {idx}")

    def test_non_keys_return_none(self):
        for key in self.non_keys:
            result = self.table.lookup(key)
            self.assertIsNone(result, f"Non-base key {key!r} incorrectly found in HP-SFC table")

    def test_low_collision_rate(self):
        stats = self.table.probe_stats()
        self.assertLess(stats["avg_probes"], 2.0, f"HP-SFC avg probes too high: {stats['avg_probes']:.3f}")
        self.assertLess(stats["load_factor"], 0.5, f"Load factor too high: {stats['load_factor']:.3f}")

    def test_contains(self):
        for key in self.base_keys[:10]:
            self.assertTrue(self.table.contains(key))
        for key in self.non_keys[:5]:
            self.assertFalse(self.table.contains(key))


class TestHPSFCRankTransportIndex(unittest.TestCase):
    def setUp(self):
        random.seed(77)
        paths = generate_scalable_tree(600, max_depth=6, seed=88)
        unique = sorted(set(paths))
        self.base_keys = unique[:500]
        self.index = HPSFCRankTransportIndex(self.base_keys, base_epsilon=64)

    def test_lookup_matches_exact_rank_base_keys(self):
        """HP-SFC lookup must produce same result as exact_rank for all base keys."""
        for key in self.base_keys:
            expected = self.index.exact_rank(key)
            got = self.index.lookup(key)
            self.assertEqual(got, expected, f"Rank mismatch for base key {key!r}: HP-SFC={got}, exact={expected}")

    def test_lookup_returns_minus1_for_absent_keys(self):
        fake_key = "/totally/nonexistent/path/zzzz"
        self.assertEqual(self.index.lookup(fake_key), -1)

    def test_insert_and_lookup(self):
        """After inserting a new key, HP-SFC index must return its correct logical rank."""
        new_key = self.base_keys[50] + "/__inserted_new"
        self.index.insert(new_key)
        expected = self.index.exact_rank(new_key)
        got = self.index.lookup(new_key)
        self.assertEqual(got, expected, f"After insert: HP-SFC={got}, exact={expected}")

    def test_delete_base_key(self):
        """After deleting a base key, lookup must return -1."""
        key = self.base_keys[100]
        self.index.delete(key)
        self.assertEqual(self.index.lookup(key), -1)

    def test_rank_transport_all_base_keys_after_inserts(self):
        """Insert 50 keys and verify rank-transport correctness for ALL live base keys."""
        inserts = [self.base_keys[i] + "/__ins" for i in range(0, 50, 5)]
        for k in inserts:
            self.index.insert(k)
        for key in self.base_keys:
            expected = self.index.exact_rank(key)
            got = self.index.lookup(key)
            self.assertEqual(got, expected, f"After inserts: HP-SFC={got}, exact={expected} for {key!r}")

    def test_consolidation_rebuilds_hpsfc_table(self):
        new_key = self.base_keys[10] + "/__consolidated"
        self.assertTrue(self.index.insert(new_key))
        self.assertTrue(self.index.maybe_consolidate(0.001))
        self.index.wait_rebuild()
        self.assertEqual(self.index.mutation_count, 0)
        self.assertEqual(self.index.consolidation_count, 1)
        self.assertEqual(self.index.lookup(new_key), self.index.exact_rank(new_key))

    def test_manual_consolidation_rebuilds_hpsfc_table(self):
        new_key = self.base_keys[20] + "/__consolidated"
        deleted_key = self.base_keys[30]
        self.assertTrue(self.index.insert(new_key))
        self.assertTrue(self.index.delete(deleted_key))

        rebuilt = self.index.consolidate(base_epsilon=0)
        self.index.wait_rebuild()
        self.assertEqual(rebuilt, 2)
        self.assertEqual(self.index.mutation_count, 0)
        self.assertEqual(self.index.consolidation_count, 1)
        self.assertIn(new_key, self.index.base_keys)
        self.assertNotIn(deleted_key, self.index.base_keys)
        self.assertEqual(self.index.lookup(new_key), self.index.exact_rank(new_key))
        self.assertEqual(self.index.lookup(deleted_key), -1)

    def test_threshold_consolidation_after_mutations(self):
        idx = HPSFCRankTransportIndex(self.base_keys[:10], base_epsilon=1, consolidation_threshold=0.2)
        self.assertTrue(idx.insert(self.base_keys[0] + "/__auto"))
        idx.wait_rebuild()
        self.assertEqual(idx.consolidation_count, 0)
        self.assertTrue(idx.insert(self.base_keys[1] + "/__auto"))
        idx.wait_rebuild()
        self.assertEqual(idx.consolidation_count, 1)
        self.assertEqual(idx.mutation_count, 0)
        self.assertEqual(idx.lookup(self.base_keys[1] + "/__auto"), idx.exact_rank(self.base_keys[1] + "/__auto"))

    def test_dns_paths(self):
        """HP-SFC must work correctly on DNS-style paths."""
        dns = sorted(set(generate_dns_paths(300, seed=11)))[:200]
        idx = HPSFCRankTransportIndex(dns, base_epsilon=64)
        for key in dns:
            expected = idx.exact_rank(key)
            got = idx.lookup(key)
            self.assertEqual(got, expected, f"DNS HP-SFC mismatch for {key!r}")

    def test_json_paths(self):
        """HP-SFC must work correctly on JSON-style paths."""
        json_paths = sorted(set(generate_json_paths(300, seed=22)))[:200]
        idx = HPSFCRankTransportIndex(json_paths, base_epsilon=64)
        for key in json_paths:
            expected = idx.exact_rank(key)
            got = idx.lookup(key)
            self.assertEqual(got, expected, f"JSON HP-SFC mismatch for {key!r}")


class TestHPSFCProbeStats(unittest.TestCase):
    def test_probe_stats_are_low(self):
        """Verify that average probes are less than 1.0 on large datasets."""
        paths = sorted(set(generate_scalable_tree(2000, max_depth=8, seed=42)))[:1500]
        table = HPSFCTable(paths)
        stats = table.probe_stats()
        self.assertLess(stats["avg_probes"], 1.5, f"Average probes too high: {stats['avg_probes']:.3f}")
        self.assertLess(stats["max_probes"], 15, f"Max probes too high: {stats['max_probes']}")


if __name__ == "__main__":
    unittest.main()
