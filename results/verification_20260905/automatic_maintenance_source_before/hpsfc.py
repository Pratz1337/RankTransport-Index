"""HP-SFC: sparse-code fingerprint table for hierarchical base-key access.

The table uses a stable hash of the exact sparse-radix lexicographic code from
``hli.lexicode``. It gives expected O(1) base-key lookup under the standard
uniform-hashing assumption, while rank transport provides the exact dynamic
rank correction for inserts and deletes.
"""

from __future__ import annotations

from bisect import bisect_left
from dataclasses import dataclass
from typing import Optional, Sequence

from hli.lexicode import lexicode_hash


@dataclass
class _Slot:
    """A single slot in the HP-SFC table."""

    key: str
    base_idx: int


class HPSFCTable:
    """
    Hierarchical Prefix Sparse Fingerprint Code table.

    A flat, open-addressed hash table indexed by a stable sparse-radix
    fingerprint of the complete path string.
    """

    _DELETED = "__DELETED__"

    def __init__(self, base_keys: list[str], load_factor: float = 0.45):
        n = len(base_keys)
        size = 1
        while size < int(n / load_factor) + 1:
            size <<= 1
        self._size = size
        self._mask = size - 1
        self._table: list[Optional[_Slot]] = [None] * size
        self._count = 0
        self._probe_counts: list[int] = []
        self._seed_cache: dict[str, tuple[int, int]] = {}

        for idx, key in enumerate(base_keys):
            self._insert_slot(key, idx)

    def _key_hash(self, key: str) -> int:
        return lexicode_hash(key)

    def _slot_from_hash(self, h: int) -> int:
        """Map a full sparse-radix path fingerprint to a table slot index."""
        return h & self._mask

    def _probe_stride_from_hash(self, h: int) -> int:
        """Compute a reproducible odd double-hash stride."""
        h ^= 0x9E3779B97F4A7C15
        return 1 + 2 * (h % ((self._size >> 1) or 1))

    def _insert_slot(self, key: str, base_idx: int) -> None:
        """Insert (key, base_idx) into the table using double hashing."""
        h = self._key_hash(key)
        slot = self._slot_from_hash(h)
        stride = self._probe_stride_from_hash(h)
        self._seed_cache[key] = (slot, stride)
        probes = 0
        while self._table[slot] is not None and self._table[slot] != self._DELETED:
            slot = (slot + stride) & self._mask
            probes += 1
            if probes >= self._size:
                raise RuntimeError("HP-SFC table is full; reduce load factor or increase table size")
        self._table[slot] = _Slot(key=key, base_idx=base_idx)
        self._count += 1
        self._probe_counts.append(probes)

    def lookup(self, key: str) -> Optional[int]:
        """
        Return the base index for a key in expected O(1), or None if absent.
        """
        seed = self._seed_cache.get(key)
        if seed is None:
            h = self._key_hash(key)
            slot = self._slot_from_hash(h)
            stride = self._probe_stride_from_hash(h)
        else:
            slot, stride = seed
        probes = 0
        while self._table[slot] is not None:
            entry = self._table[slot]
            if entry != self._DELETED and entry.key == key:
                return entry.base_idx
            slot = (slot + stride) & self._mask
            probes += 1
            if probes >= self._size:
                break
        return None

    def contains(self, key: str) -> bool:
        return self.lookup(key) is not None

    def probe_stats(self) -> dict:
        """Return probe length statistics recorded during construction."""
        if not self._probe_counts:
            return {"count": 0, "avg_probes": 0.0, "max_probes": 0, "load_factor": 0.0}
        return {
            "count": len(self._probe_counts),
            "avg_probes": sum(self._probe_counts) / len(self._probe_counts),
            "max_probes": max(self._probe_counts),
            "load_factor": self._count / self._size,
        }


class HPSFCRankTransportIndex:
    """
    HP-SFC enhanced RankTransportIndex.

    Combines:
    - HPSFCTable for expected O(1) base-key lookup
    - Signed ordered delta layer for exact rank transport of inserts/deletes
    - Certified epsilon-bound guarantee for transported base-model predictions
    """

    def __init__(
        self,
        base_keys: Sequence[str],
        base_epsilon: int,
        consolidation_threshold: Optional[float] = None,
    ):
        from hli.rank_transport import RankTransportIndex

        unique = sorted(set(base_keys))
        self._rt = RankTransportIndex(unique, base_epsilon)
        self._hpsfc = HPSFCTable(unique)
        self._base_epsilon = base_epsilon
        if consolidation_threshold is not None and consolidation_threshold <= 0:
            raise ValueError("consolidation threshold must be positive")
        self._consolidation_threshold = consolidation_threshold

    def __len__(self) -> int:
        return len(self._rt)

    def contains(self, key: str) -> bool:
        return self._rt.contains(key)

    def insert(self, key: str) -> bool:
        changed = self._rt.insert(key)
        if changed:
            self._maybe_consolidate()
        return changed

    def delete(self, key: str) -> bool:
        changed = self._rt.delete(key)
        if changed:
            self._maybe_consolidate()
        return changed

    @property
    def mutation_count(self) -> int:
        return self._rt.mutation_count

    @property
    def mutation_ratio(self) -> float:
        return self._rt.mutation_ratio

    @property
    def consolidation_count(self) -> int:
        return self._rt.consolidation_count

    @property
    def base_keys(self) -> tuple[str, ...]:
        with self._rt.lock:
            return tuple(self._rt.base_keys)

    def wait_rebuild(self) -> None:
        self._rt.wait_rebuild()

    def consolidate(self, base_epsilon: int | None = None) -> int:
        def rebuild_hpsfc(base_keys: list[str]) -> HPSFCTable:
            return HPSFCTable(base_keys)

        def publish_hpsfc(_base_keys: list[str], table: HPSFCTable) -> None:
            self._hpsfc = table

        return self._rt.consolidate(
            base_epsilon,
            rebuild_hook=rebuild_hpsfc,
            publish_hook=publish_hpsfc,
        )

    def maybe_consolidate(self, threshold: float) -> bool:
        def rebuild_hpsfc(base_keys: list[str]) -> HPSFCTable:
            return HPSFCTable(base_keys)

        def publish_hpsfc(_base_keys: list[str], table: HPSFCTable) -> None:
            self._hpsfc = table

        return self._rt.maybe_consolidate(
            threshold,
            rebuild_hook=rebuild_hpsfc,
            publish_hook=publish_hpsfc,
        )

    def _maybe_consolidate(self) -> None:
        if self._consolidation_threshold is not None:
            self.maybe_consolidate(self._consolidation_threshold)

    def lookup(self, key: str) -> int:
        """
        Return the logical rank, or -1 if absent.

        Base keys take the HP-SFC expected O(1) path. Mutated keys still need
        ordered delta ranks, so the full dynamic lookup is expected
        O(1 + log m), where m is the signed-delta size.
        """
        with self._rt.lock:
            if self._rt.delta.contains_inserted(key):
                base_less = bisect_left(self._rt.base_keys, key)
                return base_less + self._rt.delta.prefix_delta(key)

            if self._rt.delta.contains_deleted(key):
                return -1

            base_idx = self._hpsfc.lookup(key)
            if base_idx is None:
                return -1

            return base_idx + self._rt.delta.prefix_delta(key)

    def exact_rank(self, key: str) -> int:
        return self._rt.exact_rank(key)

    def verify_bound(self, predicted_base: dict[str, int]) -> dict:
        return self._rt.verify_bound(predicted_base)

    def hpsfc_stats(self) -> dict:
        return self._hpsfc.probe_stats()
