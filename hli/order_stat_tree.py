"""Order-statistic treap for dynamic rank queries over string keys."""

from __future__ import annotations

from dataclasses import dataclass
import random
from typing import Iterator, Optional


@dataclass
class _Node:
    key: str
    priority: float
    left: Optional["_Node"] = None
    right: Optional["_Node"] = None
    size: int = 1


def _size(node: Optional[_Node]) -> int:
    return node.size if node is not None else 0


def _refresh(node: Optional[_Node]) -> Optional[_Node]:
    if node is not None:
        node.size = 1 + _size(node.left) + _size(node.right)
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


class OrderStatisticTreap:
    """Set-like treap supporting rank(key) in expected O(log n)."""

    def __init__(self, seed: int = 42):
        self._root: Optional[_Node] = None
        self._rng = random.Random(seed)

    def __len__(self) -> int:
        return _size(self._root)

    def __contains__(self, key: str) -> bool:
        node = self._root
        while node is not None:
            if key == node.key:
                return True
            node = node.left if key < node.key else node.right
        return False

    def add(self, key: str) -> bool:
        if key in self:
            return False
        self._root = self._insert(self._root, _Node(key, self._rng.random()))
        return True

    def discard(self, key: str) -> bool:
        if key not in self:
            return False
        self._root = self._erase(self._root, key)
        return True

    def rank(self, key: str) -> int:
        """Return the number of stored keys strictly smaller than key."""
        node = self._root
        total = 0
        while node is not None:
            if key <= node.key:
                node = node.left
            else:
                total += 1 + _size(node.left)
                node = node.right
        return total

    def rank_le(self, key: str) -> int:
        """Return the number of stored keys smaller than or equal to key."""
        node = self._root
        total = 0
        while node is not None:
            if key < node.key:
                node = node.left
            else:
                total += 1 + _size(node.left)
                node = node.right
        return total

    def to_list(self) -> list[str]:
        return list(self)

    def __iter__(self) -> Iterator[str]:
        stack: list[_Node] = []
        node = self._root
        while stack or node is not None:
            while node is not None:
                stack.append(node)
                node = node.left
            node = stack.pop()
            yield node.key
            node = node.right

    def _insert(self, root: Optional[_Node], node: _Node) -> _Node:
        if root is None:
            return node
        if node.key < root.key:
            root.left = self._insert(root.left, node)
            if root.left is not None and root.left.priority < root.priority:
                root = _rotate_right(root)
        else:
            root.right = self._insert(root.right, node)
            if root.right is not None and root.right.priority < root.priority:
                root = _rotate_left(root)
        return _refresh(root)  # type: ignore[return-value]

    def _erase(self, root: Optional[_Node], key: str) -> Optional[_Node]:
        if root is None:
            return None
        if key < root.key:
            root.left = self._erase(root.left, key)
            return _refresh(root)
        if key > root.key:
            root.right = self._erase(root.right, key)
            return _refresh(root)
        return self._merge(root.left, root.right)

    def _merge(self, left: Optional[_Node], right: Optional[_Node]) -> Optional[_Node]:
        if left is None:
            return right
        if right is None:
            return left
        if left.priority < right.priority:
            left.right = self._merge(left.right, right)
            return _refresh(left)
        right.left = self._merge(left, right.left)
        return _refresh(right)
