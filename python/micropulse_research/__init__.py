"""MicroPulse research layer: thin Python API over the C++ engine (pybind11)."""
from ._micropulse import (  # noqa: F401
    Engine,
    simulate,
    run_cusum,
    run_bocpd,
    FLAG_WARMUP,
    FLAG_GAP_RECENT,
    FLAG_WINDOW_TAINTED,
    FLAG_BOOK_STALE,
    FLAG_TRUNCATED,
    FLAG_RING_OVERFLOW,
    FLAG_TS_REGRESSION,
    FLAG_BAR_SKIP,
)

__all__ = ["Engine", "simulate", "run_cusum", "run_bocpd"]
