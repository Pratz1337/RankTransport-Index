"""Failure/retry checks for the index's actual background generation transition."""
import threading
import unittest
from unittest.mock import patch

from hli.hpsfc import HPSFCRankTransportIndex
from hli.rank_transport import RankTransportIndex


class ConsolidationFailureTests(unittest.TestCase):
    def check_live(self, index, keys):
        expected = sorted(keys)
        self.assertEqual(index.snapshot_keys(), expected)
        for rank, key in enumerate(expected):
            self.assertTrue(index.contains(key))
            self.assertEqual(index.exact_rank(key), rank)

    def test_pending_failure_preserves_both_ledgers_before_and_after_publish(self):
        for operation, key in (("insert", "/d"), ("delete", "/c"),
                               ("delete", "/b"), ("insert", "/e")):
            for after_prepare in (False, True):
                with self.subTest(operation=operation, key=key, after_prepare=after_prepare):
                    index = RankTransportIndex(["/a", "/c", "/e"], 0)
                    index.insert("/b")
                    index.delete("/e")
                    expected = ["/a", "/b", "/c"]
                    ready, release = threading.Event(), threading.Event()

                    def hold(snapshot):
                        ready.set()
                        if not release.wait(5):
                            raise RuntimeError("test release timed out")

                    index.consolidate(rebuild_hook=hold)
                    try:
                        self.assertTrue(ready.wait(2))
                        old_active, old_pending = index.delta, index.pending_delta
                        method = f"_record_pending_{operation}_unlocked"
                        record = getattr(index, method)

                        def fail_pending(*args):
                            if after_prepare:
                                record(*args)
                            raise MemoryError("pending preparation failed")

                        with patch.object(index, method, side_effect=fail_pending):
                            with self.assertRaisesRegex(MemoryError, "pending preparation"):
                                getattr(index, operation)(key)
                        self.assertIs(index.delta, old_active)
                        self.assertIs(index.pending_delta, old_pending)
                        self.check_live(index, expected)
                    finally:
                        release.set()
                    index.wait_rebuild()
                    self.check_live(index, expected)
                    self.assertEqual(index.consolidation_count, 1)
                    self.assertTrue(getattr(index, operation)(key))
                    expected = sorted(set(expected) | {key}) if operation == "insert" else [k for k in expected if k != key]
                    self.check_live(index, expected)

    def test_successful_staged_writes_are_the_delta_published_by_worker(self):
        index = RankTransportIndex(["/a", "/c", "/e"], 0)
        index.insert("/b")
        ready, release = threading.Event(), threading.Event()

        def hold(snapshot):
            ready.set()
            if not release.wait(5):
                raise RuntimeError("test release timed out")

        index.consolidate(rebuild_hook=hold)
        try:
            self.assertTrue(ready.wait(2))
            index.insert("/d")
            index.delete("/c")
            index.delete("/b")
            index.insert("/f")
            index.delete("/f")
            expected = ["/a", "/d", "/e"]
            self.check_live(index, expected)
        finally:
            release.set()
        index.wait_rebuild()
        self.check_live(index, expected)

    def test_rebuild_failure_preserves_concurrent_writes_and_allows_retry(self):
        index = RankTransportIndex(["/a", "/c", "/e"], 1)
        index.insert("/b")
        original_base = index.base_keys
        started, release = threading.Event(), threading.Event()

        def fail_rebuild(snapshot):
            started.set()
            if not release.wait(2):
                raise RuntimeError("test release timed out")
            raise MemoryError("injected rebuild allocation failure")

        index.consolidate(base_epsilon=2, rebuild_hook=fail_rebuild)
        try:
            self.assertTrue(started.wait(2))
            index.delete("/b")
            index.delete("/c")
            index.insert("/d")
        finally:
            release.set()
        with self.assertRaisesRegex(MemoryError, "injected rebuild"):
            index.wait_rebuild()
        self.assertIs(index.base_keys, original_base)
        self.assertEqual(index.base_epsilon, 1)
        self.assertFalse(index.rebuilding)
        self.assertIsNone(index._rebuild_base_set)
        self.assertEqual(len(index.pending_delta), 0)
        self.assertEqual(index.consolidation_count, 0)
        self.check_live(index, ["/a", "/d", "/e"])
        index.consolidate(base_epsilon=2)
        index.wait_rebuild()
        self.assertEqual(index.consolidation_count, 1)
        self.assertEqual(index.mutation_count, 0)
        self.check_live(index, ["/a", "/d", "/e"])

    def test_publication_failure_does_not_replace_core_generation(self):
        index = RankTransportIndex(["/a", "/c"], 3)
        index.insert("/b")
        original_base, original_delta = index.base_keys, index.delta

        def fail_publish(snapshot, artifact):
            self.assertIs(index.base_keys, original_base)
            self.assertIs(index.delta, original_delta)
            raise RuntimeError("injected publish failure")

        index.consolidate(base_epsilon=0, publish_hook=fail_publish)
        with self.assertRaisesRegex(RuntimeError, "injected publish"):
            index.wait_rebuild()
        self.assertIs(index.base_keys, original_base)
        self.assertIs(index.delta, original_delta)
        self.assertEqual(index.base_epsilon, 3)
        self.assertFalse(index.rebuilding)
        self.check_live(index, ["/a", "/b", "/c"])
        index.consolidate()
        index.wait_rebuild()
        self.assertEqual(index.consolidation_count, 1)

    def test_thread_start_failure_restores_idle_state(self):
        index = RankTransportIndex(["/a"], 0)
        index.insert("/b")
        with patch("hli.rank_transport.threading.Thread.start", side_effect=RuntimeError("cannot start worker")):
            with self.assertRaisesRegex(RuntimeError, "cannot start worker"):
                index.consolidate()
        self.assertFalse(index.rebuilding)
        self.assertIsNone(index.rebuild_thread)
        self.assertIsNone(index._rebuild_base_set)
        self.assertEqual(len(index.pending_delta), 0)
        index.wait_rebuild()
        self.check_live(index, ["/a", "/b"])
        index.consolidate()
        index.wait_rebuild()
        self.assertEqual(index.consolidation_count, 1)

    def test_old_completion_callback_failure_cannot_reset_new_rebuild(self):
        index = RankTransportIndex(["/a"], 0)
        index.insert("/b")
        callback_started, release_callback = threading.Event(), threading.Event()
        rebuild_started, release_rebuild = threading.Event(), threading.Event()

        def fail_after_commit():
            callback_started.set()
            if not release_callback.wait(2):
                raise RuntimeError("test callback release timed out")
            raise RuntimeError("completion callback failed after commit")

        def block_new_rebuild(snapshot):
            rebuild_started.set()
            if not release_rebuild.wait(2):
                raise RuntimeError("test rebuild release timed out")

        index.consolidate(on_complete=fail_after_commit)
        old_result, old_thread = index._rebuild_result, index.rebuild_thread
        try:
            self.assertTrue(callback_started.wait(2))
            self.assertEqual(index.consolidation_count, 1)
            index.insert("/c")
            index.consolidate(rebuild_hook=block_new_rebuild)
            self.assertTrue(rebuild_started.wait(2))
            release_callback.set()
            old_thread.join(2)
            self.assertFalse(old_thread.is_alive())
            with self.assertRaisesRegex(RuntimeError, "after commit"):
                old_result.result(timeout=2)
            self.assertTrue(index.rebuilding)
            index.delete("/b")
        finally:
            release_callback.set()
            release_rebuild.set()
        index.wait_rebuild()
        self.assertEqual(index.consolidation_count, 2)
        self.check_live(index, ["/a", "/c"])

    def test_hpsfc_failed_rebuild_keeps_hash_and_rank_generation_aligned(self):
        index = HPSFCRankTransportIndex(["/a", "/c", "/e"], 0)
        index.insert("/b")
        old_hash = index._hpsfc
        with patch("hli.hpsfc.HPSFCTable", side_effect=MemoryError("hash rebuild failed")):
            index.consolidate()
            with self.assertRaisesRegex(MemoryError, "hash rebuild failed"):
                index.wait_rebuild()
        self.assertIs(index._hpsfc, old_hash)
        index.delete("/c")
        for rank, key in enumerate(["/a", "/b", "/e"]):
            self.assertEqual(index.lookup(key), rank)
            self.assertEqual(index.exact_rank(key), rank)
        self.assertEqual(index.lookup("/c"), -1)
        index.consolidate()
        index.wait_rebuild()
        for rank, key in enumerate(["/a", "/b", "/e"]):
            self.assertEqual(index.lookup(key), rank)
        self.assertEqual(index.consolidation_count, 1)


if __name__ == "__main__":
    unittest.main()
