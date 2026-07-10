import math

import pytest

from codex_buddy.usage_limits import USAGE_LIMITS_FRESHNESS_SECONDS, UsageDisplay, UsageLimits


def _window(used_percent: object, duration: object, resets_at: object) -> dict[str, object]:
    return {
        "usedPercent": used_percent,
        "windowDurationMins": duration,
        "resetsAt": resets_at,
    }


def test_complete_codex_snapshot_exports_remaining_percentages():
    limits = UsageLimits.from_read_result(
        {
            "rateLimitsByLimitId": {
                "codex": {
                    "primary": _window(28, 300, 10),
                    "secondary": _window(9, 10080, 20),
                }
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)


def test_legacy_snapshot_is_used_when_the_codex_limit_bucket_is_unavailable():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(12.6, 300, 10),
                "secondary": _window(47.5, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=100.0) == UsageDisplay(five_hour_remaining=87, seven_day_remaining=53)


def test_sparse_primary_update_retains_secondary_window_and_primary_metadata():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 300, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    updated = limits.merge_update(
        {"rateLimits": {"primary": {"usedPercent": 40}}},
        observed_at=125.0,
    )

    assert updated.primary is not None
    assert updated.primary.window_duration_mins == 300
    assert updated.primary.resets_at == 10
    assert updated.secondary == limits.secondary
    assert updated.display_pair(now=126.0) == UsageDisplay(five_hour_remaining=60, seven_day_remaining=91)


@pytest.mark.parametrize(
    "used_percent",
    [float("nan"), float("inf"), -0.1, 100.1, "28", True],
)
def test_malformed_nonfinite_or_out_of_range_usage_never_exports_a_pair(used_percent: object):
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(used_percent, 300, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) is None


def test_unsupported_window_duration_never_exports_a_pair():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 60, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) is None


def test_expired_snapshot_is_suppressed_after_the_named_freshness_limit():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 300, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=100.0 + USAGE_LIMITS_FRESHNESS_SECONDS) == UsageDisplay(
        five_hour_remaining=72,
        seven_day_remaining=91,
    )
    assert limits.display_pair(now=101.0 + USAGE_LIMITS_FRESHNESS_SECONDS) is None


def test_invalid_sparse_field_suppresses_the_merged_pair_without_losing_the_other_window():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 300, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    updated = limits.merge_update(
        {"rateLimits": {"primary": {"usedPercent": math.inf}}},
        observed_at=125.0,
    )

    assert updated.primary is None
    assert updated.secondary == limits.secondary
    assert updated.display_pair(now=126.0) is None
