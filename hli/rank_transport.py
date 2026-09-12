"""Exact dynamic rank correction for learned indexes over hierarchical keys."""

from __future__ import annotations

from bisect import bisect_left
from concurrent.futures import Future
from dataclasses import dataclass
import threading
from typing import Mapping, Sequence

from hli.signed_delta import SignedDeltaTreap


@dataclass(frozen=True)
class LookupCertificate:
    key: str
    found: bool
    source: str
    exact_rank: int | None
    predicted_rank: int | None
    error: int | None
    low: int | None
    high: int | None


class RankTransportIndex:
    """
    Wrap a static learned rank model with exact dynamic rank deltas.

    For a live base key k:
        rank_t(k) = rank_0(k) + delta_before(k)

    Therefore a base-model prediction p_0(k), with initial error <= epsilon,
    transports to p_t(k) with the same error bound after any insert/delete
    sequence, as long as delta ranks are exact.
    """

    def __init__(self, base_keys: Sequence[str], base_epsilon: int):
        unique = sorted(set(base_keys))
        if len(unique) != len(base_keys):
            raise ValueError("RankTransportIndex requires unique base keys")
        self._delta_seed = 1337
        self.base_keys = unique
        self.base_epsilon = int(base_epsilon)
        self.delta = SignedDeltaTreap(seed=self._delta_seed)
        
        # Concurrency fields
        self.lock = threading.RLock()
        self.rebuilding = False
        self.rebuild_thread: threading.Thread | None = None
        self._rebuild_result: Future[None] | None = None
        self.pending_delta = SignedDeltaTreap(seed=self._delta_seed)
        self._rebuild_base_set: set[str] | None = None
        
        self.consolidation_count = 0

    def wait_rebuild(self) -> None:
        """Wait for the observed rebuild, propagating its worker/callback failure."""
        with self.lock:
            thread = self.rebuild_thread
            result = self._rebuild_result
        if thread is not None and thread is not threading.current_thread():
            thread.join()
            if result is not None:
                result.result()

    def __len__(self) -> int:
        with self.lock:
            return len(self.base_keys) - self.delta.deleted_count + self.delta.inserted_count

    @property
    def mutation_count(self) -> int:
        with self.lock:
            return len(self.delta)

    @property
    def mutation_ratio(self) -> float:
        with self.lock:
            if not self.base_keys:
                return float("inf") if self.mutation_count else 0.0
            return self.mutation_count / len(self.base_keys)

    def should_consolidate(self, threshold: float) -> bool:
        with self.lock:
            if threshold <= 0:
                raise ValueError("consolidation threshold must be positive")
            return self.mutation_count > 0 and self.mutation_ratio >= threshold

    def consolidate(
        self,
        base_epsilon: int | None = None,
        rebuild_hook=None,
        publish_hook=None,
        on_complete=None,
    ) -> int:
        """
        Rebuild the live key snapshot in a background thread.

        Writes that arrive while the rebuild is running are recorded against the
        rebuilding snapshot and atomically published as the next delta layer.
        Each write prepares path-copied active and pending ledgers before either
        is installed, so preparation failure changes neither visible state.
        Rebuild/publication failure retains the old generation and all writes;
        wait_rebuild() reports the error. Hooks must not mutate the supplied
        snapshot or call index mutations. The publish hook runs under the index
        lock before the core generation is replaced and must itself be exception
        atomic: it must leave external state unchanged if it raises. A failing
        on_complete callback is reported after an already committed generation.
        """
        with self.lock:
            if self.rebuilding:
                return 0
            mutations = self.mutation_count
            if mutations == 0:
                if base_epsilon is not None:
                    self.base_epsilon = int(base_epsilon)
                return 0
            
            snapshot = self._snapshot_keys_unlocked()
            snapshot_base = set(snapshot)
            next_epsilon = self.base_epsilon if base_epsilon is None else int(base_epsilon)

            pending_delta = SignedDeltaTreap(seed=self._delta_seed)
            empty_delta = SignedDeltaTreap(seed=self._delta_seed)
            result: Future[None] = Future()
            next_count = self.consolidation_count + 1

            def rebuild_worker():
                try:
                    artifact = rebuild_hook(snapshot) if rebuild_hook is not None else None
                    with self.lock:
                        # Publish the prepared side index first. Its exception-
                        # atomic hook may fail without replacing the live base.
                        if publish_hook is not None:
                            publish_hook(snapshot, artifact)
                        self.base_keys = snapshot
                        self.base_epsilon = next_epsilon
                        self.delta = self.pending_delta
                        self.pending_delta = empty_delta
                        self._rebuild_base_set = None
                        self.rebuilding = False
                        self.consolidation_count = next_count
                except BaseException as error:
                    with self.lock:
                        self.pending_delta = empty_delta
                        self._rebuild_base_set = None
                        self.rebuilding = False
                    result.set_exception(error)
                    return
                # This callback is outside the commit. A newer rebuild may be
                # running by now, so its state must not be reset on callback error.
                try:
                    if on_complete:
                        on_complete()
                except BaseException as error:
                    result.set_exception(error)
                else:
                    result.set_result(None)

            thread = threading.Thread(target=rebuild_worker)
            self.rebuilding = True
            self._rebuild_base_set = snapshot_base
            self.pending_delta = pending_delta
            self.rebuild_thread = thread
            self._rebuild_result = result
            try:
                thread.start()
            except BaseException:
                self.pending_delta = empty_delta
                self._rebuild_base_set = None
                self.rebuilding = False
                self.rebuild_thread = None
                self._rebuild_result = None
                raise
            return mutations

    def maybe_consolidate(
        self,
        threshold: float,
        rebuild_hook=None,
        publish_hook=None,
        on_complete=None,
    ) -> bool:
        with self.lock:
            if self.rebuilding:
                return False
            if not self.should_consolidate(threshold):
                return False
            return self.consolidate(
                rebuild_hook=rebuild_hook,
                publish_hook=publish_hook,
                on_complete=on_complete,
            ) > 0

    def contains_base(self, key: str) -> bool:
        with self.lock:
            idx = bisect_left(self.base_keys, key)
            return idx < len(self.base_keys) and self.base_keys[idx] == key

    def contains(self, key: str) -> bool:
        with self.lock:
            if self.delta.contains_inserted(key):
                return True
            return self.contains_base(key) and not self.delta.contains_deleted(key)

    def insert(self, key: str) -> bool:
        """Insert key, or restore a deleted base key. Returns True if state changed."""
        with self.lock:
            delta = self.delta.fork() if self.rebuilding else self.delta
            if self.contains_base(key):
                res = delta.discard_deleted(key)
            else:
                res = delta.mark_inserted(key)
            if res and self.rebuilding:
                pending = self.pending_delta.fork()
                self._record_pending_insert_unlocked(key, pending)
                # Both paths are prepared before publishing either root.
                self.delta = delta
                self.pending_delta = pending
            return res

    def delete(self, key: str) -> bool:
        """Delete a live key. Returns True if state changed."""
        with self.lock:
            delta = self.delta.fork() if self.rebuilding else self.delta
            res = False
            if delta.discard_inserted(key):
                res = True
            elif self.contains_base(key) and not delta.contains_deleted(key):
                res = delta.mark_deleted(key)
            if res and self.rebuilding:
                pending = self.pending_delta.fork()
                self._record_pending_delete_unlocked(key, pending)
                self.delta = delta
                self.pending_delta = pending
            return res

    def delta_before(self, key: str) -> int:
        """Net rank shift contributed by mutations before key."""
        with self.lock:
            return self.delta.prefix_delta(key)

    def exact_rank(self, key: str) -> int:
        with self.lock:
            if not self.contains(key):
                raise KeyError(key)
            base_less = bisect_left(self.base_keys, key)
            return base_less + self.delta.prefix_delta(key)

    def exact_rank_if_present(self, key: str) -> int | None:
        with self.lock:
            return self.exact_rank(key) if self.contains(key) else None

    def transport_base_prediction(self, key: str, predicted_base_rank: int) -> int:
        with self.lock:
            if not self.contains_base(key):
                raise KeyError(f"{key!r} is not a base-model key")
            return int(predicted_base_rank) + self.delta_before(key)

    def lookup(self, key: str, predicted_base_rank: int) -> int:
        """
        Lookup a key and return its logical rank in the index, or -1 if not found.
        Avoids storing base_positions map.
        """
        with self.lock:
            if self.delta.contains_inserted(key):
                base_less = bisect_left(self.base_keys, key)
                return base_less + self.delta.prefix_delta(key)

            if self.delta.contains_deleted(key):
                return -1

            # Search range in the base_keys array (not the logical space)
            low = max(0, predicted_base_rank - self.base_epsilon)
            high = min(len(self.base_keys) - 1, predicted_base_rank + self.base_epsilon)

            idx = bisect_left(self.base_keys, key, low, high + 1)
            if idx <= high and idx < len(self.base_keys) and self.base_keys[idx] == key:
                return idx + self.delta.prefix_delta(key)

            return -1

    def lookup_certificate(self, key: str, predicted_base_rank: int | None = None) -> LookupCertificate:
        """
        Return a proof object for the lookup path.

        Inserted keys bypass the neural model and use the exact delta-rank
        structure. Base keys use transported model predictions and the original
        epsilon window.
        """
        with self.lock:
            if self.delta.contains_inserted(key):
                rank = self.exact_rank(key)
                return LookupCertificate(key, True, "delta", rank, rank, 0, rank, rank)

            idx = bisect_left(self.base_keys, key)
            in_base = idx < len(self.base_keys) and self.base_keys[idx] == key
            if not in_base or self.delta.contains_deleted(key):
                return LookupCertificate(key, False, "absent", None, None, None, None, None)

            if predicted_base_rank is None:
                predicted_base_rank = idx
            exact = self.exact_rank(key)
            pred = self.transport_base_prediction(key, predicted_base_rank)
            low = max(0, pred - self.base_epsilon)
            high = min(len(self) - 1, pred + self.base_epsilon)
            return LookupCertificate(key, True, "transported-base", exact, pred, abs(pred - exact), low, high)

    def verify_bound(self, predicted_base_ranks: Mapping[str, int]) -> dict[str, float | int | bool]:
        with self.lock:
            max_error = 0
            checked = 0
            for key in self.base_keys:
                if self.delta.contains_deleted(key):
                    continue
                pred0 = predicted_base_ranks[key]
                cert = self.lookup_certificate(key, pred0)
                assert cert.error is not None
                max_error = max(max_error, cert.error)
                checked += 1
            return {
                "checked_base_keys": checked,
                "current_size": len(self),
                "inserted_keys": self.delta.inserted_count,
                "deleted_base_keys": self.delta.deleted_count,
                "max_transported_error": max_error,
                "base_epsilon": self.base_epsilon,
                "bound_holds": max_error <= self.base_epsilon,
            }

    def snapshot_keys(self) -> list[str]:
        """Materialize current key order for testing and offline evaluation."""
        with self.lock:
            return self._snapshot_keys_unlocked()

    def _snapshot_keys_unlocked(self) -> list[str]:
        deleted = set(self.delta.to_deleted_list())
        merged = [key for key in self.base_keys if key not in deleted]
        merged.extend(self.delta.to_inserted_list())
        return sorted(merged)

    def _record_pending_insert_unlocked(self, key: str, pending: SignedDeltaTreap) -> None:
        if self._rebuild_base_set is not None and key in self._rebuild_base_set:
            pending.discard_deleted(key)
        else:
            pending.mark_inserted(key)

    def _record_pending_delete_unlocked(self, key: str, pending: SignedDeltaTreap) -> None:
        if self._rebuild_base_set is not None and key in self._rebuild_base_set:
            pending.mark_deleted(key)
        else:
            pending.discard_inserted(key)
