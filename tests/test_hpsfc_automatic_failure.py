"""Keep committed write results separate from automatic maintenance failures."""
import unittest
from unittest.mock import patch

from hli.hpsfc import HPSFCRankTransportIndex


class AutomaticConsolidationFailureTests(unittest.TestCase):
    def check_live(self, index, expected):
        self.assertEqual(index._rt.snapshot_keys(), expected)
        for rank, key in enumerate(expected):
            self.assertTrue(index.contains(key))
            self.assertEqual(index.lookup(key), rank)
            self.assertEqual(index.exact_rank(key), rank)

    def test_committed_writes_report_startup_failures_through_wait_and_allow_retry(self):
        for operation, key, initial_expected in (("insert", "/b", ["/a", "/b", "/c"]),
                                         ("delete", "/c", ["/a"])):
            for failure in ("snapshot", "thread"):
                for retry in ("manual", "threshold", "automatic"):
                    with self.subTest(operation=operation, failure=failure, retry=retry):
                        expected = initial_expected.copy()
                        index = HPSFCRankTransportIndex(["/a", "/c"], 0, 0.5)
                        old_hash, old_base = index._hpsfc, index._rt.base_keys
                        error = MemoryError("snapshot failed") if failure == "snapshot" else RuntimeError("worker start failed")
                        target = patch.object(index._rt, "_snapshot_keys_unlocked", side_effect=error) if failure == "snapshot" else patch("hli.rank_transport.threading.Thread.start", side_effect=error)
                        with target:
                            try:
                                changed = getattr(index, operation)(key)
                            except Exception as raised:
                                self.fail(f"committed {operation} incorrectly raised {raised!r}")
                        self.assertTrue(changed)
                        self.assertIs(index._hpsfc, old_hash)
                        self.assertIs(index._rt.base_keys, old_base)
                        self.assertFalse(index._rt.rebuilding)
                        self.check_live(index, expected)
                        with self.assertRaises(type(error)) as caught:
                            index.wait_rebuild()
                        self.assertIs(caught.exception, error)
                        # A no-op write must not silently erase the failed attempt.
                        self.assertFalse(getattr(index, operation)(key))
                        with self.assertRaises(type(error)):
                            index.wait_rebuild()
                        if retry == "manual":
                            self.assertGreater(index.consolidate(), 0)
                        elif retry == "threshold":
                            self.assertTrue(index.maybe_consolidate(0.5))
                        else:
                            self.assertTrue(index.insert("/d"))
                            expected = sorted(expected + ["/d"])
                        index.wait_rebuild()
                        self.assertEqual(index.consolidation_count, 1)
                        self.assertEqual(index.mutation_count, 0)
                        self.check_live(index, expected)

    def test_explicit_startup_failure_still_raises_synchronously(self):
        for operation in ("consolidate", "maybe_consolidate"):
            with self.subTest(operation=operation):
                index = HPSFCRankTransportIndex(["/a", "/c"], 0)
                self.assertTrue(index.insert("/b"))
                with patch("hli.rank_transport.threading.Thread.start", side_effect=RuntimeError("worker start failed")):
                    with self.assertRaisesRegex(RuntimeError, "worker start failed"):
                        getattr(index, operation)(*([0.5] if operation == "maybe_consolidate" else []))
                self.check_live(index, ["/a", "/b", "/c"])
                index.consolidate()
                index.wait_rebuild()
                self.assertEqual(index.consolidation_count, 1)

    def test_worker_failure_after_startup_retry_is_not_hidden(self):
        index = HPSFCRankTransportIndex(["/a", "/c"], 0, 0.5)
        with patch("hli.rank_transport.threading.Thread.start", side_effect=RuntimeError("worker start failed")):
            self.assertTrue(index.insert("/b"))
        with self.assertRaisesRegex(RuntimeError, "worker start failed"):
            index.wait_rebuild()
        with patch("hli.hpsfc.HPSFCTable", side_effect=MemoryError("hash build failed")):
            index.consolidate()
            with self.assertRaisesRegex(MemoryError, "hash build failed"):
                index.wait_rebuild()
        self.check_live(index, ["/a", "/b", "/c"])
        index.consolidate()
        index.wait_rebuild()
        self.assertEqual(index.consolidation_count, 1)

    def test_write_preparation_failure_is_not_swallowed(self):
        for operation in ("insert", "delete"):
            with self.subTest(operation=operation):
                index = HPSFCRankTransportIndex(["/a", "/c"], 0, 0.5)
                with patch.object(index._rt, operation, side_effect=MemoryError("write failed")):
                    with self.assertRaisesRegex(MemoryError, "write failed"):
                        getattr(index, operation)("/a")
                index.wait_rebuild()
                self.check_live(index, ["/a", "/c"])

    def test_process_control_exception_is_not_swallowed(self):
        index = HPSFCRankTransportIndex(["/a", "/c"], 0, 0.5)
        with patch.object(index._rt, "_snapshot_keys_unlocked", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                index.insert("/b")
        # Process-control interruption is explicitly outside the write guarantee.
        self.check_live(index, ["/a", "/b", "/c"])


if __name__ == "__main__":
    unittest.main()
