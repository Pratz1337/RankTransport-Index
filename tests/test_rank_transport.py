import bisect
import random
import threading
import unittest

from hli.order_stat_tree import OrderStatisticTreap
from hli.rank_transport import RankTransportIndex
from hli.signed_delta import SignedDeltaTreap


class OrderStatisticTreapTests(unittest.TestCase):
    def test_rank_matches_sorted_list_after_mutations(self):
        rng = random.Random(7)
        treap = OrderStatisticTreap(seed=7)
        mirror: list[str] = []

        keys = [f"/k/{i:03d}" for i in range(80)]
        for key in rng.sample(keys, 60):
            self.assertEqual(treap.add(key), key not in mirror)
            if key not in mirror:
                mirror.insert(bisect.bisect_left(mirror, key), key)

        for key in keys:
            self.assertEqual(treap.rank(key), bisect.bisect_left(mirror, key))

        for key in rng.sample(keys, 30):
            changed = treap.discard(key)
            if key in mirror:
                mirror.pop(bisect.bisect_left(mirror, key))
                self.assertTrue(changed)
            else:
                self.assertFalse(changed)

        self.assertEqual(treap.to_list(), mirror)


class SignedDeltaTreapTests(unittest.TestCase):
    def test_prefix_delta_matches_insert_minus_delete_ranks(self):
        delta = SignedDeltaTreap(seed=11)
        for key in ["/b", "/d", "/f"]:
            self.assertTrue(delta.mark_inserted(key))
        for key in ["/c", "/e"]:
            self.assertTrue(delta.mark_deleted(key))

        probes = ["/a", "/b", "/c", "/d", "/e", "/f", "/g"]
        for key in probes:
            inserted_before = sum(1 for item in ["/b", "/d", "/f"] if item < key)
            deleted_before = sum(1 for item in ["/c", "/e"] if item < key)
            self.assertEqual(delta.prefix_delta(key), inserted_before - deleted_before)

    def test_annihilation_removes_mutation_entry(self):
        delta = SignedDeltaTreap(seed=11)
        self.assertTrue(delta.mark_deleted("/b"))
        self.assertTrue(delta.contains_deleted("/b"))
        self.assertTrue(delta.discard_deleted("/b"))
        self.assertEqual(len(delta), 0)
        self.assertEqual(delta.prefix_delta("/z"), 0)

    def test_random_signed_updates_match_dict_model(self):
        rng = random.Random(19)
        delta = SignedDeltaTreap(seed=19)
        mirror: dict[str, int] = {}
        keys = [f"/k/{i:03d}" for i in range(60)]

        for _ in range(300):
            key = rng.choice(keys)
            op = rng.choice(["mark_inserted", "mark_deleted", "discard_inserted", "discard_deleted"])
            before = mirror.get(key)

            changed = getattr(delta, op)(key)
            if op == "mark_inserted":
                expected_changed = before != 1
                mirror[key] = 1
            elif op == "mark_deleted":
                expected_changed = before != -1
                mirror[key] = -1
            elif op == "discard_inserted":
                expected_changed = before == 1
                if expected_changed:
                    del mirror[key]
            else:
                expected_changed = before == -1
                if expected_changed:
                    del mirror[key]

            self.assertEqual(changed, expected_changed)
            self.assertEqual(len(delta), len(mirror))
            self.assertEqual(delta.inserted_count, sum(1 for weight in mirror.values() if weight > 0))
            self.assertEqual(delta.deleted_count, sum(1 for weight in mirror.values() if weight < 0))
            for probe in rng.sample(keys, 8):
                expected_prefix = sum(weight for item, weight in mirror.items() if item < probe)
                self.assertEqual(delta.prefix_delta(probe), expected_prefix)


class RankTransportIndexTests(unittest.TestCase):
    def test_transported_error_bound_survives_inserts_and_deletes(self):
        base_keys = [f"/root/{c}" for c in "abcdefghij"]
        predicted = {
            key: max(0, min(len(base_keys) - 1, i + (1 if i % 3 == 0 else 0)))
            for i, key in enumerate(base_keys)
        }
        index = RankTransportIndex(base_keys, base_epsilon=1)

        for key in ["/root/a/child", "/root/c/child", "/root/h/child", "/root/z"]:
            self.assertTrue(index.insert(key))
        self.assertTrue(index.delete("/root/b"))
        self.assertTrue(index.delete("/root/g"))

        summary = index.verify_bound(predicted)
        self.assertTrue(summary["bound_holds"])
        self.assertLessEqual(summary["max_transported_error"], 1)

        for key in base_keys:
            if index.contains(key):
                cert = index.lookup_certificate(key, predicted[key])
                self.assertTrue(cert.found)
                self.assertEqual(cert.source, "transported-base")
                self.assertIsNotNone(cert.error)
                self.assertLessEqual(cert.error, 1)

    def test_inserted_keys_have_exact_delta_rank(self):
        base_keys = ["/root/a", "/root/c", "/root/e"]
        index = RankTransportIndex(base_keys, base_epsilon=0)

        self.assertTrue(index.insert("/root/b"))
        self.assertTrue(index.insert("/root/d"))
        self.assertEqual(index.snapshot_keys(), ["/root/a", "/root/b", "/root/c", "/root/d", "/root/e"])

        cert = index.lookup_certificate("/root/d")
        self.assertTrue(cert.found)
        self.assertEqual(cert.source, "delta")
        self.assertEqual(cert.exact_rank, 3)
        self.assertEqual(cert.error, 0)

    def test_base_delete_and_restore(self):
        index = RankTransportIndex(["/a", "/b", "/c"], base_epsilon=0)
        self.assertTrue(index.delete("/b"))
        self.assertFalse(index.contains("/b"))
        self.assertEqual(index.snapshot_keys(), ["/a", "/c"])

        self.assertTrue(index.insert("/b"))
        self.assertTrue(index.contains("/b"))
        self.assertEqual(index.snapshot_keys(), ["/a", "/b", "/c"])

    def test_consolidation_rebuilds_base_and_resets_delta(self):
        index = RankTransportIndex(["/a", "/c", "/e"], base_epsilon=1)
        self.assertTrue(index.insert("/b"))
        self.assertTrue(index.delete("/c"))
        self.assertTrue(index.should_consolidate(0.5))

        rebuilt = index.consolidate(base_epsilon=2)
        index.wait_rebuild()

        self.assertEqual(rebuilt, 2)
        self.assertEqual(index.consolidation_count, 1)
        self.assertEqual(index.mutation_count, 0)
        self.assertEqual(index.base_epsilon, 2)
        self.assertEqual(index.snapshot_keys(), ["/a", "/b", "/e"])
        self.assertEqual(index.exact_rank("/b"), 1)
        self.assertFalse(index.contains("/c"))

    def test_post_consolidation_uses_fresh_base_certificate(self):
        index = RankTransportIndex(["/a", "/d", "/g"], base_epsilon=0)
        self.assertTrue(index.insert("/b"))
        self.assertTrue(index.insert("/e"))
        self.assertTrue(index.delete("/d"))

        self.assertEqual(index.consolidate(base_epsilon=0), 3)
        index.wait_rebuild()

        predicted = {key: rank for rank, key in enumerate(index.base_keys)}
        summary = index.verify_bound(predicted)
        self.assertTrue(summary["bound_holds"])
        self.assertEqual(summary["max_transported_error"], 0)
        self.assertEqual(index.mutation_count, 0)
        self.assertEqual(index.base_keys, ["/a", "/b", "/e", "/g"])

    def test_consolidation_absorbs_live_deltas(self):
        index = RankTransportIndex(["/a", "/c", "/e"], base_epsilon=1)

        self.assertTrue(index.insert("/b"))
        self.assertTrue(index.insert("/d"))
        self.assertTrue(index.delete("/c"))
        self.assertTrue(index.should_consolidate(1.0))

        rebuilt = index.consolidate(base_epsilon=0)
        index.wait_rebuild()
        self.assertEqual(rebuilt, 3)
        self.assertEqual(index.consolidation_count, 1)
        self.assertEqual(index.mutation_count, 0)
        self.assertEqual(index.base_epsilon, 0)
        self.assertEqual(index.base_keys, ["/a", "/b", "/d", "/e"])

        predicted = {key: rank for rank, key in enumerate(index.base_keys)}
        summary = index.verify_bound(predicted)
        self.assertTrue(summary["bound_holds"])
        self.assertEqual(summary["max_transported_error"], 0)
        for rank, key in enumerate(index.base_keys):
            self.assertEqual(index.lookup(key, predicted[key]), rank)

    def test_concurrent_mutations_during_rebuild(self):
        index = RankTransportIndex(["/1", "/3", "/5"], base_epsilon=1)
        self.assertTrue(index.insert("/2"))
        self.assertTrue(index.insert("/4"))
        
        # Trigger rebuild
        self.assertTrue(index.maybe_consolidate(0.5))
        
        # Concurrently perform mutation and verify containment
        self.assertTrue(index.insert("/6"))
        self.assertTrue(index.contains("/6"))
        self.assertTrue(index.contains("/2"))
        self.assertTrue(index.contains("/4"))
        
        # Wait for rebuild to finish
        index.wait_rebuild()
        
        # After rebuild, /2 and /4 should be absorbed into base, /6 remains in active delta
        self.assertTrue(index.contains("/6"))
        self.assertTrue(index.contains("/2"))
        self.assertTrue(index.contains("/4"))
        self.assertEqual(index.mutation_count, 1)

    def test_delete_absorbed_insert_during_background_rebuild_stays_deleted(self):
        index = RankTransportIndex(["/1", "/3", "/5"], base_epsilon=1)
        self.assertTrue(index.insert("/2"))
        self.assertTrue(index.insert("/4"))
        started = threading.Event()
        release = threading.Event()
        snapshots: list[list[str]] = []

        def block_rebuild(snapshot):
            snapshots.append(list(snapshot))
            started.set()
            release.wait(2)

        self.assertEqual(index.consolidate(rebuild_hook=block_rebuild), 2)
        self.assertTrue(started.wait(2), "background rebuild did not start")
        self.assertIn("/2", snapshots[0])

        self.assertTrue(index.delete("/2"))
        self.assertFalse(index.contains("/2"))
        self.assertTrue(index.insert("/6"))
        release.set()
        index.wait_rebuild()

        self.assertFalse(index.contains("/2"))
        self.assertTrue(index.contains("/4"))
        self.assertTrue(index.contains("/6"))
        self.assertEqual(index.snapshot_keys(), ["/1", "/3", "/4", "/5", "/6"])
        self.assertEqual(index.mutation_count, 2)


if __name__ == "__main__":
    unittest.main()
