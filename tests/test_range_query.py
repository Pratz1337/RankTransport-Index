"""
Tests for range query correctness under dynamic updates.
Verifies Theorem 4: count_range = rank_after(hi) - rank_at_or_after(lo)
matches brute-force scan under arbitrary insertions and deletions.
"""

import random
import unittest

from hli.datasets import generate_scalable_tree, generate_dns_paths
from hli.range_query import RangeQueryIndex


class TestRangeQueryStatic(unittest.TestCase):
    """Range queries on static (no mutations) index."""

    def setUp(self):
        random.seed(42)
        paths = sorted(set(generate_scalable_tree(500, max_depth=6, seed=11)))[:300]
        self.keys = paths
        self.index = RangeQueryIndex(paths, base_epsilon=64)

    def test_count_all_keys(self):
        """Range [min, max] must return all N keys."""
        lo, hi = self.keys[0], self.keys[-1]
        self.assertEqual(self.index.count_range(lo, hi), len(self.keys))

    def test_empty_range(self):
        """Range lo > hi must return 0."""
        self.assertEqual(self.index.count_range("z", "a"), 0)

    def test_single_key_ranges(self):
        """Range [k, k] must return 1 for each base key."""
        for k in self.keys[:20]:
            result = self.index.count_range(k, k)
            self.assertEqual(result, 1, f"count_range([{k}, {k}]) = {result}, expected 1")

    def test_range_matches_brute_force(self):
        """count_range must match len(scan_range) for 50 random ranges."""
        for _ in range(50):
            i, j = sorted(random.sample(range(len(self.keys)), 2))
            lo, hi = self.keys[i], self.keys[j]
            v = self.index.verify_range_correctness(lo, hi)
            self.assertTrue(v["match"],
                f"Range [{lo}, {hi}]: fast={v['fast_count']}, brute={v['brute_count']}")

    def test_scan_range_content(self):
        """scan_range must return exactly the base keys in [lo, hi]."""
        lo, hi = self.keys[50], self.keys[100]
        expected = sorted(k for k in self.keys if lo <= k <= hi)
        got = self.index.scan_range(lo, hi)
        self.assertEqual(got, expected)

    def test_range_nonexistent_boundaries(self):
        """Range with boundaries not in the key set must still be correct."""
        lo = self.keys[10] + "\x00"   # just after keys[10]
        hi = self.keys[20]
        expected_count = sum(1 for k in self.keys if lo <= k <= hi)
        self.assertEqual(self.index.count_range(lo, hi), expected_count)


class TestRangeQueryDynamic(unittest.TestCase):
    """Range queries after dynamic insertions and deletions."""

    def setUp(self):
        random.seed(77)
        paths = sorted(set(generate_scalable_tree(500, max_depth=6, seed=22)))[:300]
        self.keys = paths
        self.index = RangeQueryIndex(paths, base_epsilon=64)

    def test_range_after_inserts(self):
        """count_range must be correct after inserting 30 new keys."""
        new_keys = [self.keys[i] + "/__new" for i in range(0, 30)]
        for k in new_keys:
            self.index.insert(k)

        for _ in range(30):
            i, j = sorted(random.sample(range(len(self.keys)), 2))
            lo, hi = self.keys[i], self.keys[j]
            v = self.index.verify_range_correctness(lo, hi)
            self.assertTrue(v["match"],
                f"After inserts, range [{lo}, {hi}]: fast={v['fast_count']}, brute={v['brute_count']}")

    def test_range_after_deletes(self):
        """count_range must be correct after deleting 20 base keys."""
        to_delete = self.keys[::15][:20]
        for k in to_delete:
            self.index.delete(k)

        for _ in range(30):
            i, j = sorted(random.sample(range(len(self.keys)), 2))
            lo, hi = self.keys[i], self.keys[j]
            v = self.index.verify_range_correctness(lo, hi)
            self.assertTrue(v["match"],
                f"After deletes, range [{lo}, {hi}]: fast={v['fast_count']}, brute={v['brute_count']}")

    def test_range_mixed_mutations(self):
        """count_range correct after 50 inserts and 20 deletes interleaved."""
        inserts = [self.keys[i] + "/__ins" for i in range(0, 50)]
        for k in inserts:
            self.index.insert(k)
        for k in self.keys[5:25]:
            self.index.delete(k)

        for _ in range(50):
            i, j = sorted(random.sample(range(len(self.keys)), 2))
            lo, hi = self.keys[i], self.keys[j]
            v = self.index.verify_range_correctness(lo, hi)
            self.assertTrue(v["match"],
                f"Mixed mutations, range [{lo}, {hi}]: fast={v['fast_count']}, brute={v['brute_count']}")

    def test_inserted_keys_appear_in_scan(self):
        """Inserted keys must appear in scan_range when in range."""
        new_key = self.keys[100] + "/__check"
        self.index.insert(new_key)
        lo = self.keys[95]
        hi = self.keys[105]
        scan = self.index.scan_range(lo, hi)
        self.assertIn(new_key, scan, f"Inserted key {new_key} not found in scan [{lo}, {hi}]")

    def test_deleted_keys_absent_from_scan(self):
        """Deleted base keys must not appear in scan_range."""
        del_key = self.keys[150]
        self.index.delete(del_key)
        lo = self.keys[145]
        hi = self.keys[155]
        scan = self.index.scan_range(lo, hi)
        self.assertNotIn(del_key, scan, f"Deleted key {del_key} still found in scan")

    def test_scan_range_matches_snapshot_after_random_mutations(self):
        """scan_range and count_range match the live snapshot after mixed churn."""
        rng = random.Random(123)
        inserted: list[str] = []
        for _ in range(80):
            base = rng.choice(self.keys)
            key = f"{base}/__rand_{rng.randrange(1000):04d}"
            if self.index.insert(key):
                inserted.append(key)

        for key in rng.sample(self.keys, 45):
            self.index.delete(key)
        for key in rng.sample(inserted, min(20, len(inserted))):
            self.index.delete(key)

        snapshot = self.index._rt.snapshot_keys()
        probes = sorted(set(self.keys + inserted))
        for _ in range(100):
            lo, hi = sorted(rng.sample(probes, 2))
            expected = [key for key in snapshot if lo <= key <= hi]
            self.assertEqual(self.index.scan_range(lo, hi), expected)
            self.assertEqual(self.index.count_range(lo, hi), len(expected))

    def test_dns_range_queries(self):
        """Range queries on DNS paths must be correct."""
        dns = sorted(set(generate_dns_paths(300, seed=33)))[:200]
        idx = RangeQueryIndex(dns, base_epsilon=64)
        # Insert 20 new DNS keys
        for k in dns[:20]:
            idx.insert(k + ".tmp")
        for _ in range(30):
            i, j = sorted(random.sample(range(len(dns)), 2))
            lo, hi = dns[i], dns[j]
            v = idx.verify_range_correctness(lo, hi)
            self.assertTrue(v["match"],
                f"DNS range [{lo}, {hi}]: fast={v['fast_count']}, brute={v['brute_count']}")


if __name__ == "__main__":
    unittest.main()
