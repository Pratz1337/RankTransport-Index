"""Signed order-statistic delta tree for rank transport."""

from __future__ import annotations

from dataclasses import dataclass, replace
import random
from typing import Iterator, Optional


@dataclass
class _Node:
    key: str
    weight: int
    priority: float
    left: Optional["_Node"] = None
    right: Optional["_Node"] = None
    size: int = 1
    subtree_sum: int = 0
    inserted_count: int = 0
    deleted_count: int = 0


def _size(node: Optional[_Node]) -> int:
    return node.size if node is not None else 0


def _sum(node: Optional[_Node]) -> int:
    return node.subtree_sum if node is not None else 0


def _inserted_count(node: Optional[_Node]) -> int:
    return node.inserted_count if node is not None else 0


def _deleted_count(node: Optional[_Node]) -> int:
    return node.deleted_count if node is not None else 0


def _refresh(node: Optional[_Node]) -> Optional[_Node]:
    if node is not None:
        node.size = 1 + _size(node.left) + _size(node.right)
        node.subtree_sum = node.weight + _sum(node.left) + _sum(node.right)
        node.inserted_count = (1 if node.weight > 0 else 0) + _inserted_count(node.left) + _inserted_count(node.right)
        node.deleted_count = (1 if node.weight < 0 else 0) + _deleted_count(node.left) + _deleted_count(node.right)
    return node


def _rotate_right(root: _Node) -> _Node:
    new_root = root.left
    assert new_root is not None
    root.left = new_root.right
    new_root.right = root
    _refresh(root)
    return _refresh(new_root)  # type: ignore[return-value]


def _rotate_left(root: _Node) -> _Node:
    new_root = root.right
    assert new_root is not None
    root.right = new_root.left
    new_root.left = root
    _refresh(root)
    return _refresh(new_root)  # type: ignore[return-value]


class SignedDeltaTreap:
    """Signed mutation tree with path-copying updates and atomic root replacement."""

    def __init__(self, seed: int = 42):
        self._root: Optional[_Node] = None
        self._rng = random.Random(seed)

    def fork(self) -> SignedDeltaTreap:
        """Share immutable nodes for a staged update; priorities share an RNG.

        Callers must synchronize branches. Failed preparation can consume a
        priority, but cannot change either branch's keys, counts or prefix sums.
        """
        branch = object.__new__(SignedDeltaTreap)
        branch._root = self._root
        branch._rng = self._rng
        return branch

    def __len__(self) -> int:
        return _size(self._root)

    @property
    def inserted_count(self) -> int:
        return _inserted_count(self._root)

    @property
    def deleted_count(self) -> int:
        return _deleted_count(self._root)

    def contains_inserted(self, key: str) -> bool:
        node = self._find(key)
        return node is not None and node.weight > 0

    def contains_deleted(self, key: str) -> bool:
        node = self._find(key)
        return node is not None and node.weight < 0

    def mark_inserted(self, key: str) -> bool:
        return self._set_weight(key, 1)

    def mark_deleted(self, key: str) -> bool:
        return self._set_weight(key, -1)

    def discard_inserted(self, key: str) -> bool:
        self._root, erased = self._erase_weight(self._root, key, 1)
        return erased

    def discard_deleted(self, key: str) -> bool:
        self._root, erased = self._erase_weight(self._root, key, -1)
        return erased

    def prefix_delta(self, key: str) -> int:
        """Return inserted_before(key) - deleted_before(key)."""
        node = self._root
        total = 0
        while node is not None:
            if key <= node.key:
                node = node.left
            else:
                total += _sum(node.left) + node.weight
                node = node.right
        return total

    def rank_inserted(self, key: str) -> int:
        node = self._root
        total = 0
        while node is not None:
            if key <= node.key:
                node = node.left
            else:
                total += _inserted_count(node.left) + (1 if node.weight > 0 else 0)
                node = node.right
        return total

    def rank_deleted(self, key: str) -> int:
        node = self._root
        total = 0
        while node is not None:
            if key <= node.key:
                node = node.left
            else:
                total += _deleted_count(node.left) + (1 if node.weight < 0 else 0)
                node = node.right
        return total

    def rank_delta_le(self, key: str) -> int:
        node = self._root
        total = 0
        while node is not None:
            if key < node.key:
                node = node.left
            else:
                total += _sum(node.left) + node.weight
                node = node.right
        return total

    def to_inserted_list(self) -> list[str]:
        return [key for key, weight in self._items() if weight > 0]

    def to_deleted_list(self) -> list[str]:
        return [key for key, weight in self._items() if weight < 0]

    def _find(self, key: str) -> Optional[_Node]:
        node = self._root
        while node is not None:
            if key == node.key:
                return node
            node = node.left if key < node.key else node.right
        return None

    def _set_weight(self, key: str, weight: int) -> bool:
        self._root, changed = self._insert_or_update(self._root, key, weight)
        return changed

    def _insert_or_update(self, root: Optional[_Node], key: str, weight: int) -> tuple[Optional[_Node], bool]:
        if root is None:
            return _refresh(_Node(key, weight, self._rng.random())), True
        root = replace(root)
        if key == root.key:
            if root.weight == weight:
                return root, False
            root.weight = weight
            return _refresh(root), True
        if key < root.key:
            left, changed = self._insert_or_update(root.left, key, weight)
            root.left = left
            if left is not None and left.priority < root.priority:
                root = _rotate_right(root)
        else:
            right, changed = self._insert_or_update(root.right, key, weight)
            root.right = right
            if right is not None and right.priority < root.priority:
                root = _rotate_left(root)
        return _refresh(root), changed

    def _erase_weight(self, root: Optional[_Node], key: str, weight: int) -> tuple[Optional[_Node], bool]:
        if root is None:
            return None, False
        root = replace(root)
        if key < root.key:
            left, erased = self._erase_weight(root.left, key, weight)
            root.left = left
            return _refresh(root), erased
        if key > root.key:
            right, erased = self._erase_weight(root.right, key, weight)
            root.right = right
            return _refresh(root), erased
        
        # key == root.key
        if root.weight != weight:
            return root, False
        return self._merge(root.left, root.right), True

    def _merge(self, left: Optional[_Node], right: Optional[_Node]) -> Optional[_Node]:
        if left is None:
            return right
        if right is None:
            return left
        if left.priority < right.priority:
            left = replace(left)
            left.right = self._merge(left.right, right)
            return _refresh(left)
        right = replace(right)
        right.left = self._merge(left, right.left)
        return _refresh(right)

    def _items(self) -> Iterator[tuple[str, int]]:
        stack: list[_Node] = []
        node = self._root
        while stack or node is not None:
            while node is not None:
                stack.append(node)
                node = node.left
            node = stack.pop()
            yield node.key, node.weight
            node = node.right

    def iter_range(self, lo: str, hi: str) -> Iterator[tuple[str, int]]:
        """In-order traversal of entries with lo <= key <= hi. O(log n + k)."""
        stack: list[_Node] = []
        node = self._root
        while stack or node is not None:
            while node is not None:
                if node.key >= lo:
                    stack.append(node)
                    node = node.left
                else:
                    # Skip left subtree entirely — all keys < lo
                    node = node.right
            if not stack:
                break
            node = stack.pop()
            if node.key > hi:
                break
            yield node.key, node.weight
            node = node.right
