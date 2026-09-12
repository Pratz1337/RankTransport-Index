"""Prepared-node allocation failures must never mutate a published treap root."""
from contextlib import ExitStack
import random
import unittest
from unittest.mock import patch

import hli.signed_delta as delta_module
from hli.signed_delta import SignedDeltaTreap


class SignedDeltaTransactionTests(unittest.TestCase):
    def assert_tree(self, tree, expected):
        self.assertEqual(dict(tree._items()), expected)
        self.assertEqual(len(tree), len(expected))
        self.assertEqual(tree.inserted_count, sum(v > 0 for v in expected.values()))
        self.assertEqual(tree.deleted_count, sum(v < 0 for v in expected.values()))
        for boundary in ["", *expected, "/zz"]:
            self.assertEqual(tree.prefix_delta(boundary), sum(v for k, v in expected.items() if k < boundary))

        def check(node):
            if node is None:
                return 0, 0
            left_n, left_sum = check(node.left)
            right_n, right_sum = check(node.right)
            self.assertEqual(node.size, left_n + right_n + 1)
            self.assertEqual(node.subtree_sum, left_sum + right_sum + node.weight)
            for child in (node.left, node.right):
                if child is not None:
                    self.assertGreaterEqual(child.priority, node.priority)
            return node.size, node.subtree_sum
        check(tree._root)

    def test_forks_remain_independent_through_rotations_and_erasures(self):
        original = SignedDeltaTreap(seed=19)
        expected = {f"/{i:03}": 1 if i % 2 else -1 for i in range(80)}
        for key, weight in expected.items():
            original._set_weight(key, weight)
        left, right = original.fork(), original.fork()
        left_expected, right_expected = dict(expected), dict(expected)
        rng = random.Random(811)
        for step in range(500):
            tree, target = (left, left_expected) if step % 2 else (right, right_expected)
            key = f"/{rng.randrange(140):03}"
            weight = rng.choice([-1, 0, 1])
            if weight:
                tree._set_weight(key, weight)
                target[key] = weight
            elif key in target:
                tree.discard_inserted(key) if target[key] > 0 else tree.discard_deleted(key)
                del target[key]
            self.assert_tree(tree, target)
            self.assert_tree(original, expected)
        self.assert_tree(left, left_expected)
        self.assert_tree(right, right_expected)

    def test_every_preparation_failure_preserves_published_tree(self):
        scenarios = [
            ("mark_inserted", "/new"), ("mark_deleted", "/new"),
            ("mark_deleted", "/013"), ("mark_inserted", "/014"),
            ("discard_inserted", "/013"), ("discard_deleted", "/014"),
        ]
        for operation, key in scenarios:
            with self.subTest(operation=operation, key=key):
                failed_points = 0
                for fail_at in range(1, 200):
                    tree = SignedDeltaTreap(seed=19)
                    expected = {f"/{i:03}": 1 if i % 2 else -1 for i in range(40)}
                    for stored_key, weight in expected.items():
                        tree._set_weight(stored_key, weight)
                    old_root = tree._root
                    calls = 0

                    def fault(function):
                        def injected(*args, **kwargs):
                            nonlocal calls
                            calls += 1
                            if calls == fail_at:
                                raise MemoryError("prepared node failure")
                            return function(*args, **kwargs)
                        return injected

                    with ExitStack() as stack:
                        for name in ("replace", "_Node", "_refresh"):
                            stack.enter_context(patch.object(delta_module, name, fault(getattr(delta_module, name))))
                        try:
                            getattr(tree, operation)(key)
                        except MemoryError:
                            failed = True
                        else:
                            failed = False
                    if not failed:
                        self.assertGreater(failed_points, 0)
                        break
                    failed_points += 1
                    self.assertIs(tree._root, old_root)
                    self.assert_tree(tree, expected)
                    tree.mark_inserted("/retry")
                    expected["/retry"] = 1
                    self.assert_tree(tree, expected)
                else:
                    self.fail("allocation sweep did not reach a successful operation")


if __name__ == "__main__":
    unittest.main()
