"""Validated Codex account-rate-limit snapshots for the Buddy display."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import math
from numbers import Real
from typing import Optional


FIVE_HOUR_WINDOW_MINUTES = 5 * 60
SEVEN_DAY_WINDOW_MINUTES = 7 * 24 * 60
WINDOW_DURATION_TOLERANCE_MINUTES = 5
USAGE_LIMITS_FRESHNESS_SECONDS = 15 * 60

_MISSING = object()


@dataclass(frozen=True)
class UsageWindow:
    used_percent: float
    window_duration_mins: float
    resets_at: float


@dataclass(frozen=True)
class UsageDisplay:
    five_hour_remaining: int
    seven_day_remaining: int


@dataclass(frozen=True)
class UsageLimits:
    primary: Optional[UsageWindow]
    secondary: Optional[UsageWindow]
    observed_at: Optional[float]

    @classmethod
    def from_read_result(cls, result: object, *, observed_at: object) -> "UsageLimits":
        rate_limits = _rate_limits_payload(result)
        if rate_limits is None:
            return cls(primary=None, secondary=None, observed_at=_finite_number(observed_at))
        return cls(
            primary=_complete_window(rate_limits.get("primary")),
            secondary=_complete_window(rate_limits.get("secondary")),
            observed_at=_finite_number(observed_at),
        )

    def merge_update(self, update: object, *, observed_at: object) -> "UsageLimits":
        rate_limits = _rate_limits_payload(update)
        if rate_limits is None or not ({"primary", "secondary"} & set(rate_limits)):
            return self

        primary = self.primary
        secondary = self.secondary
        if "primary" in rate_limits:
            primary = _merge_window(self.primary, rate_limits["primary"])
        if "secondary" in rate_limits:
            secondary = _merge_window(self.secondary, rate_limits["secondary"])

        return UsageLimits(
            primary=primary,
            secondary=secondary,
            observed_at=_finite_number(observed_at),
        )

    def display_pair(self, *, now: object) -> Optional[UsageDisplay]:
        now_value = _finite_number(now)
        if now_value is None or self.observed_at is None:
            return None
        if now_value - self.observed_at > USAGE_LIMITS_FRESHNESS_SECONDS:
            return None
        if not _matches_duration(self.primary, FIVE_HOUR_WINDOW_MINUTES):
            return None
        if not _matches_duration(self.secondary, SEVEN_DAY_WINDOW_MINUTES):
            return None

        assert self.primary is not None
        assert self.secondary is not None
        return UsageDisplay(
            five_hour_remaining=_remaining_percent(self.primary.used_percent),
            seven_day_remaining=_remaining_percent(self.secondary.used_percent),
        )


def _rate_limits_payload(payload: object) -> Optional[Mapping[str, object]]:
    if not isinstance(payload, Mapping):
        return None

    by_limit_id = payload.get("rateLimitsByLimitId")
    if isinstance(by_limit_id, Mapping) and "codex" in by_limit_id:
        codex_limits = by_limit_id["codex"]
        return codex_limits if isinstance(codex_limits, Mapping) else None

    legacy_limits = payload.get("rateLimits")
    return legacy_limits if isinstance(legacy_limits, Mapping) else None


def _complete_window(payload: object) -> Optional[UsageWindow]:
    if not isinstance(payload, Mapping):
        return None
    return _make_window(
        payload.get("usedPercent", _MISSING),
        payload.get("windowDurationMins", _MISSING),
        payload.get("resetsAt", _MISSING),
    )


def _merge_window(existing: Optional[UsageWindow], payload: object) -> Optional[UsageWindow]:
    if not isinstance(payload, Mapping):
        return None

    used_percent = existing.used_percent if existing is not None else _MISSING
    window_duration_mins = existing.window_duration_mins if existing is not None else _MISSING
    resets_at = existing.resets_at if existing is not None else _MISSING
    return _make_window(
        payload.get("usedPercent", used_percent),
        payload.get("windowDurationMins", window_duration_mins),
        payload.get("resetsAt", resets_at),
    )


def _make_window(used_percent: object, window_duration_mins: object, resets_at: object) -> Optional[UsageWindow]:
    used_value = _finite_number(used_percent)
    duration_value = _finite_number(window_duration_mins)
    reset_value = _finite_number(resets_at)
    if used_value is None or not 0.0 <= used_value <= 100.0:
        return None
    if duration_value is None or duration_value <= 0.0:
        return None
    if reset_value is None or reset_value < 0.0:
        return None
    return UsageWindow(
        used_percent=used_value,
        window_duration_mins=duration_value,
        resets_at=reset_value,
    )


def _matches_duration(window: Optional[UsageWindow], expected_minutes: int) -> bool:
    if window is None:
        return False
    return abs(window.window_duration_mins - expected_minutes) <= WINDOW_DURATION_TOLERANCE_MINUTES


def _remaining_percent(used_percent: float) -> int:
    rounded = math.floor((100.0 - used_percent) + 0.5)
    return max(0, min(100, int(rounded)))


def _finite_number(value: object) -> Optional[float]:
    if isinstance(value, bool) or not isinstance(value, Real):
        return None
    number = float(value)
    return number if math.isfinite(number) else None
