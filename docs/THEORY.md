# The exactness contract

This note restates the definitions and results behind HRT-LI ("Count, Don't Retrain",
Nelapati and Sayal). Implementation: `hrtli_cpp/packed_rank_transport.hpp` (frozen model,
bracket check) and `hrtli_cpp/prefix_radix_delta.hpp` (signed byte-radix ledger).

## Setting

`S_0` is a sorted set of distinct byte strings (the *base*) and `S_t` the live set after the
successful writes up to time `t`. For any set `X` and boundary `k`,
`rank_X(k) = |{x ∈ X : x < k}|`; write `rank_0 = rank_{S_0}` and `rank_t = rank_{S_t}`.
A model `p_0` is *certified at ε* if

$$\max_{k\in S_0} |p_0(k) - \mathrm{rank}_0(k)| \le \varepsilon \qquad (1)$$

verified on every base key. The model is never changed afterwards; only an explicit rebuild
ends its validity.

## Order

All comparisons are unsigned and bytewise. Map `k = b_1 … b_m` to the 257-symbol word
`L(k) = (b_1+1, …, b_m+1, 0)`.

**Proposition (order).** `a < b` bytewise iff `L(a) < L(b)` lexicographically.

*Proof.* At a first differing byte, adding one on both sides keeps the inequality. If `a` is a
proper prefix of `b`, then `L(a)` has `0` where `L(b)` has a symbol ≥ 1, so `a` precedes `b` in
both orders. ∎

The shift is conceptual: the ledger stores raw bytes on its edges and a separate terminal flag,
so byte 255 never meets the terminator.

## Ledger

Let `w_t(x) = 1[x ∈ S_t] − 1[x ∈ S_0]`. Only inserted non-base keys carry `+1` and only deleted
base keys carry `−1`; every other string has weight zero and is not stored. Define

$$\Delta^{<}_t(k)=\sum_{x<k} w_t(x),\qquad \Delta^{\le}_t(k)=\sum_{x\le k} w_t(x)=\Delta^{<}_t(k)+w_t(k).$$

**Lemma (ledger invariant).** After any sequence of successful inserts and deletes, the stored
weights equal `w_t`, and `rank_t(k) = rank_0(k) + Δ^<_t(k)` for every boundary `k`, stored or not.

*Proof.* Initially both sides are zero. A successful insert of a non-base key, or a delete of a
base key, flips one indicator and stores ±1; re-inserting a deleted base key, or deleting a
pending insert, restores the indicator and removes the entry. Hence the ledger tracks the
current difference, not the history. Summing `1_{S_t} = 1_{S_0} + w_t` over `x < k` gives the
rank identity. ∎

**Theorem (transport).** Assume (1), a common comparator for `S_0` and `S_t`, and no counter
overflow. For every surviving base key `k ∈ S_0 ∩ S_t`, the shifted prediction
`p_t(k) = p_0(k) + Δ^<_t(k)` satisfies `|p_t(k) − rank_t(k)| = |p_0(k) − rank_0(k)| ≤ ε`.

*Proof.* Subtract the lemma's identity from the definition of `p_t`; the correction cancels. ∎

Keys outside `S_0` are not covered, and for them the index never consults the certificate.

**Proposition (range count).** For `lo ≤ hi`, the number of live keys in `[lo, hi]` equals

$$\big(\mathrm{rank}_0(hi)+\mathbf 1[hi\in S_0]\big)-\mathrm{rank}_0(lo)+\Delta^{\le}_t(hi)-\Delta^{<}_t(lo),$$

whether or not `lo` and `hi` are stored, live or deleted.

*Proof.* The count is `|{x ∈ S_t : x ≤ hi}| − rank_t(lo)`. The first term equals
`Σ_{x ≤ hi} (1_{S_0} + w_t)(x)`, which is the bracketed base term plus `Δ^≤_t(hi)`; the second
follows from the lemma. ∎

## Exact recovery for any boundary

The model is certified only on stored keys, so recovery of `rank_0(k)` for arbitrary `k`
searches the clipped window `[p − ε, p + ε + 1)` around the prediction `p` and accepts a
position `i` only if

$$(i=0 \lor S_0[i-1]<k)\ \land\ (i=|S_0| \lor S_0[i]\ge k). \qquad (2)$$

Because `S_0` is sorted and duplicate-free, the predicate `S_0[j] ≥ k` is monotone in `j` and
(2) holds at exactly one index, `rank_0(k)`. A passed bracket proves correctness; a failed one
triggers a search of the routed segment or the full base. For stored keys, (1) guarantees the
bracket passes inside the window. `figures/fig1_overview.pdf` traces both query kinds on a
five-key toy base.
