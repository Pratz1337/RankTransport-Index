"""
hli/range_query.py
==================
Range query support for the HRT-LI index.

Implements Theorem 4: For any query range [lo, hi], the correct count of
live keys in [lo, hi] is rk_t(hi+) - rk_t(lo), computable in
O(log N + log N_delta) time using the rank-transport layer.

The current literature notes did not find the same range-query framing for
hierarchical string learned indexes: a provably correct rank-transport
certificate under dynamic updates. This is a bounded novelty claim, not proof
that no missed related work exists.
"""

from __future__ import annotations

from bisect import bisect_left, bisect_right
from dataclasses import dataclass
from typing import Iterator, Optional, Sequence

from hli.rank_transport import RankTransportIndex


@dataclass
class RangeResult:
    """Result of a range query with provenance."""
    lo: str
    hi: str
    count: int
    base_hits: int
    delta_hits: int
    range_rank_lo: int
    range_rank_hi: int


class RangeQueryIndex:
    """
    Wraps a RankTransportIndex to support exact range queries.

    Query: count_range(lo, hi) → number of live keys k s.t. lo ≤ k ≤ hi
    Query: scan_range(lo, hi) → sorted list of all such keys

    Correctness (Theorem 4):
        count_range(lo, hi) = rk_t(hi, inclusive) - rk_t(lo, exclusive)
        where rk_t uses the full rank-transport formula.

    Complexity:
        count_range: O(log N + log N_delta)
        scan_range:  O(log N + log N_delta + k), k = result size
    """

    def __init__(self, base_keys: Sequence[str], base_epsilon: int):
        unique = sorted(set(base_keys))
        self._rt = RankTransportIndex(unique, base_epsilon)
        self._base_keys = unique
        self._epsilon = base_epsilon

    # ── Mutation API ──────────────────────────────────────────────────────────

    def insert(self, key: str) -> bool:
        return self._rt.insert(key)

    def delete(self, key: str) -> bool:
        return self._rt.delete(key)

    def __len__(self) -> int:
        return len(self._rt)

    # ── Range Query API ───────────────────────────────────────────────────────

    def rank_at_or_after(self, key: str) -> int:
        """
        Returns the logical rank of the smallest live key ≥ key.
        Equivalent to: number of live keys strictly less than key.
        O(log N + log N_delta)
        """
        base_less = bisect_left(self._base_keys, key)
        return base_less + self._rt.delta.prefix_delta(key)

    def rank_after(self, key: str) -> int:
        """
        Returns number of live keys strictly less than or equal to key.
        Equivalent to: rank_at_or_after(key) + (1 if key is live else 0)
        O(log N + log N_delta)
        """
        base_leq = bisect_right(self._base_keys, key)
        return base_leq + self._rt.delta.rank_delta_le(key)

    def count_range(self, lo: str, hi: str) -> int:
        """
        Returns the exact count of live keys k with lo ≤ k ≤ hi.
        Implements Theorem 4: count = rk_t(hi+) - rk_t(lo-)
        O(log N + log N_delta)
        """
        if lo > hi:
            return 0
        return self.rank_after(hi) - self.rank_at_or_after(lo)

    def scan_range(self, lo: str, hi: str) -> list[str]:
        """
        Returns sorted list of all live keys k with lo ≤ k ≤ hi.

        Uses a two-way merge-iterator over the base-key array and the
        delta-insert entries, skipping tombstoned base keys.
        Complexity: O(log N + log m + k), where k = result size.
        """
        return list(self.iter_range(lo, hi))

    def iter_range(self, lo: str, hi: str) -> Iterator[str]:
        """
        Merge-iterator yielding live keys in [lo, hi] in sorted order.

        Advances two cursors in lockstep:
        - base_cursor: base_keys[lo_idx:hi_idx]
        - delta_cursor: signed delta entries in [lo, hi] with weight > 0

        Base keys whose key appears as a deleted entry (weight < 0) in the
        delta tree are skipped. The result is yielded in sorted order without
        materializing the full delta list.

        Complexity: O(log N + log m + k)
        """
        if lo > hi:
            return

        # Base-key cursor
        lo_idx = bisect_left(self._base_keys, lo)
        hi_idx = bisect_right(self._base_keys, hi)
        base_pos = lo_idx

        # Delta cursor — only entries in [lo, hi]
        delta_iter = self._rt.delta.iter_range(lo, hi)
        delta_key: Optional[str] = None
        delta_weight: int = 0
        delta_exhausted = False

        def _advance_delta():
            nonlocal delta_key, delta_weight, delta_exhausted
            try:
                delta_key, delta_weight = next(delta_iter)
            except StopIteration:
                delta_key = None
                delta_weight = 0
                delta_exhausted = True

        _advance_delta()

        while base_pos < hi_idx or not delta_exhausted:
            base_key = self._base_keys[base_pos] if base_pos < hi_idx else None

            if base_key is not None and (delta_exhausted or base_key < delta_key):
                # If tombstoned, the delta cursor would be at the same key.
                yield base_key
                base_pos += 1
            elif delta_key is not None and (base_key is None or delta_key < base_key):
                # Delta entry comes first — yield if it's an insert
                if delta_weight > 0:
                    yield delta_key
                _advance_delta()
            elif base_key is not None and delta_key is not None and base_key == delta_key:
                # Same key in both — delta determines status
                if delta_weight > 0:
                    # Re-inserted base key or insert matching base: yield base
                    yield base_key
                # If delta_weight < 0, this base key is deleted — skip
                base_pos += 1
                _advance_delta()
            else:
                # Both exhausted
                break

    def range_result(self, lo: str, hi: str) -> RangeResult:
        """Returns a detailed result object with provenance."""
        if lo > hi:
            return RangeResult(lo, hi, 0, 0, 0, 0, 0)

        rk_lo = self.rank_at_or_after(lo)
        rk_hi = self.rank_after(hi)
        count = rk_hi - rk_lo

        # Count breakdown over the exact scan result.
        base_hits = 0
        delta_hits = 0
        for k in self.iter_range(lo, hi):
            idx = bisect_left(self._base_keys, k)
            if idx < len(self._base_keys) and self._base_keys[idx] == k:
                base_hits += 1
            else:
                delta_hits += 1

        return RangeResult(lo, hi, count, base_hits, delta_hits, rk_lo, rk_hi)

    def verify_range_correctness(self, lo: str, hi: str) -> dict:
        """
        Verification: compare count_range with brute-force snapshot scan.
        Returns a dict with both counts and whether they match.
        """
        fast = self.count_range(lo, hi)
        # Brute-force: materialize full snapshot and count in range
        all_keys = self._rt.snapshot_keys()
        brute = sum(1 for k in all_keys if lo <= k <= hi)
        return {
            "lo": lo, "hi": hi,
            "fast_count": fast,
            "brute_count": brute,
            "match": fast == brute,
        }
