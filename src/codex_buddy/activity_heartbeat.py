from __future__ import annotations

import math


_WINDOW_SECONDS = 20


class ActivityHeartbeat:
    """Projects real Codex activity timestamps into a compact 20-second mask."""

    def __init__(self) -> None:
        self._seconds: set[int] = set()

    def record(self, timestamp: float) -> None:
        if not math.isfinite(timestamp) or timestamp < 0:
            return
        self._seconds.add(int(timestamp))

    def mask(self, now: float) -> int:
        if not math.isfinite(now) or now < 0:
            return 0
        current = int(now)
        result = 0
        retained: set[int] = set()
        for event_second in self._seconds:
            age = current - event_second
            if 0 <= age < _WINDOW_SECONDS:
                result |= 1 << age
                retained.add(event_second)
            elif event_second > current:
                retained.add(event_second)
        self._seconds = retained
        return result
