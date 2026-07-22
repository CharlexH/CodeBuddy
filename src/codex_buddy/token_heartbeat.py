from __future__ import annotations

import base64
import math


WINDOW_SECONDS = 20.0
BIN_COUNT = 64
BIN_SECONDS = WINDOW_SECONDS / BIN_COUNT
MAX_RAW_VALUE = (1 << 63) - 1
_MAX_DELTA = MAX_RAW_VALUE * 5
_LOG_REFERENCE = math.log1p(32_000)


class TokenHeartbeat:
    """Tracks recent positive per-session token deltas as a rolling curve."""

    def __init__(self) -> None:
        self._baselines: dict[str, int] = {}
        self._source_deltas: dict[int, int] = {}
        self._available = False

    @property
    def available(self) -> bool:
        """Whether a positive cumulative token counter has been observed."""
        return self._available

    def observe(self, session_id: str, total_tokens: int, now: float) -> None:
        """Record a cumulative token counter without creating reset spikes."""
        if not session_id or not math.isfinite(now) or now < 0 or total_tokens < 0:
            return

        total = int(total_tokens)
        if total > 0:
            self._available = True
        previous = self._baselines.get(session_id)
        self._baselines[session_id] = total
        if previous is None or total <= previous:
            return

        source_bin = self._bin_at(now)
        delta = min(total - previous, _MAX_DELTA)
        self._source_deltas[source_bin] = min(
            _MAX_DELTA, self._source_deltas.get(source_bin, 0) + delta
        )

    def retain_sessions(self, session_ids: set[str]) -> None:
        """Discard baselines for sessions that are no longer present."""
        self._baselines = {
            session_id: total
            for session_id, total in self._baselines.items()
            if session_id in session_ids
        }

    def encoded(self, now: float) -> str:
        """Return 64 fixed-scale intensities as unpadded URL-safe base64."""
        samples = bytes(self._intensity(value) for value in self._raw_values(now))
        return base64.urlsafe_b64encode(samples).rstrip(b"=").decode("ascii")

    def _raw_values(self, now: float) -> tuple[int, ...]:
        if not math.isfinite(now) or now < 0:
            return (0,) * BIN_COUNT

        current_bin = self._bin_at(now)
        oldest_bin = current_bin - BIN_COUNT + 1
        values = [0] * BIN_COUNT
        retained: dict[int, int] = {}
        for source_bin, delta in self._source_deltas.items():
            if source_bin < oldest_bin:
                continue
            retained[source_bin] = delta
            for offset, value in enumerate(self._smooth(delta)):
                target_bin = source_bin + offset
                if oldest_bin <= target_bin <= current_bin:
                    index = target_bin - oldest_bin
                    values[index] = min(MAX_RAW_VALUE, values[index] + value)
        self._source_deltas = retained
        return tuple(values)

    @staticmethod
    def _smooth(delta: int) -> tuple[int, int, int]:
        first = delta // 5
        second = (delta * 3) // 5
        return first, second, delta - first - second

    @staticmethod
    def _intensity(value: int) -> int:
        if value <= 0:
            return 0
        return round(min(1.0, math.log1p(value) / _LOG_REFERENCE) * 255)

    @staticmethod
    def _bin_at(now: float) -> int:
        return math.floor(now / BIN_SECONDS)
