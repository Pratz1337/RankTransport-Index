"""Sparse-radix lexicographic codes for hierarchical path keys."""

from __future__ import annotations

from decimal import Decimal, localcontext
from functools import lru_cache


LEXICODE_RADIX = 1024
LEXICODE_FEATURE_BYTES = 256
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


@lru_cache(maxsize=200_000)
def sparse_lex_codes(path: str, max_bytes: int | None = None) -> tuple[int, ...]:
    """Return a sparse-radix code sequence for bytewise path order."""
    raw = path.encode("utf-8")
    if max_bytes is not None:
        raw = raw[:max_bytes]
    return tuple(byte + 1 for byte in raw) + (0,)


def compare_lexicode(left: str, right: str) -> int:
    """Compare two paths using their exact sparse-radix code sequences."""
    a = sparse_lex_codes(left)
    b = sparse_lex_codes(right)
    return (a > b) - (a < b)


def lexicode_to_decimal(path: str, max_bytes: int = 80, precision: int = 220) -> Decimal:
    """
    Convert a bounded prefix of the sparse code to a high-precision fraction.

    Exact ordering should use sparse_lex_codes. This decimal form is only a
    bounded geometric/model feature.
    """
    codes = sparse_lex_codes(path, max_bytes=max_bytes)
    with localcontext() as ctx:
        ctx.prec = precision
        radix = Decimal(LEXICODE_RADIX)
        value = Decimal(0)
        denom = radix
        for code in codes:
            value += Decimal(code) / denom
            denom *= radix
        return value


def lexicode_unit_float(path: str, max_bytes: int = 24) -> float:
    """Return a stable [0, 1) floating feature derived from the sparse code."""
    return float(lexicode_to_decimal(path, max_bytes=max_bytes, precision=96))


def lexicode_hash(path: str) -> int:
    """Stable 64-bit FNV-1a hash over sparse-radix code units."""
    h = FNV_OFFSET
    for byte in path.encode("utf-8"):
        code = byte + 1
        h ^= code & 0xFF
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
        h ^= (code >> 8) & 0xFF
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    h ^= 0
    h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    h ^= 0
    h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h
