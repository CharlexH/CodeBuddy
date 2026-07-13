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


def test_current_codex_shape_exports_the_week_window_without_inventing_five_hours():
    limits = UsageLimits.from_read_result(
        {
            "rateLimitsByLimitId": {
                "codex": {
                    "primary": _window(1, 10_080, 1_700_100_000),
                    "secondary": None,
                }
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(seven_day_remaining=99)


def test_five_hour_only_shape_exports_only_the_known_window():
    limits = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(28, 300, 10), "secondary": None}},
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(five_hour_remaining=72)


def test_windows_are_classified_by_duration_even_when_the_slots_are_reversed():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(9, 10_080, 20),
                "secondary": _window(28, 300, 10),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)


def test_unknown_only_window_has_no_display():
    limits = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(28, 15, 10), "secondary": None}},
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) is None


def test_missing_duration_cannot_be_classified_but_reset_time_is_optional():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": {"usedPercent": 28},
                "secondary": {"usedPercent": 9, "windowDurationMins": 10_080},
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(seven_day_remaining=91)


def test_full_read_is_authoritative_and_removes_a_previous_window():
    limits = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(28, 300, 10), "secondary": _window(9, 10_080, 20)}},
        observed_at=100.0,
    )

    updated = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(10, 10_080, 30), "secondary": None}},
        observed_at=125.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)
    assert updated.display_pair(now=126.0) == UsageDisplay(seven_day_remaining=90)


def test_sparse_explicit_null_clears_only_that_slot():
    limits = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(28, 300, 10), "secondary": _window(9, 10_080, 20)}},
        observed_at=100.0,
    )

    updated = limits.merge_update(
        {"rateLimits": {"primary": None}},
        observed_at=125.0,
    )

    assert updated.display_pair(now=126.0) == UsageDisplay(seven_day_remaining=91)


def test_sparse_slot_duration_change_reclassifies_that_slot():
    limits = UsageLimits.from_read_result(
        {"rateLimits": {"primary": _window(28, 300, 10), "secondary": None}},
        observed_at=100.0,
    )

    updated = limits.merge_update(
        {"rateLimits": {"primary": {"usedPercent": 9, "windowDurationMins": 10_080}}},
        observed_at=125.0,
    )

    assert updated.display_pair(now=126.0) == UsageDisplay(seven_day_remaining=91)


def test_duplicate_slots_for_one_duration_fail_closed_for_that_window():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 300, 10),
                "secondary": _window(9, 300, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) is None


def test_non_codex_limit_buckets_are_never_used_as_codex_usage():
    limits = UsageLimits.from_read_result(
        {
            "rateLimitsByLimitId": {
                "codex": {"primary": _window(1, 10_080, 10), "secondary": None},
                "codex_bengalfox": {
                    "primary": _window(80, 300, 10),
                    "secondary": _window(70, 10_080, 20),
                },
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(seven_day_remaining=99)


def test_usage_display_requires_at_least_one_known_window():
    with pytest.raises(ValueError, match="window"):
        UsageDisplay()


@pytest.mark.parametrize("value", [-1, 101, 72.5, True, "72"])
def test_usage_display_rejects_non_integer_or_out_of_range_values(value: object):
    with pytest.raises(ValueError, match="percentage"):
        UsageDisplay(seven_day_remaining=value)  # type: ignore[arg-type]


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
def test_malformed_nonfinite_or_out_of_range_slot_does_not_hide_the_other_window(used_percent: object):
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(used_percent, 300, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(seven_day_remaining=91)


def test_unsupported_window_duration_is_ignored_without_hiding_a_known_window():
    limits = UsageLimits.from_read_result(
        {
            "rateLimits": {
                "primary": _window(28, 60, 10),
                "secondary": _window(9, 10080, 20),
            }
        },
        observed_at=100.0,
    )

    assert limits.display_pair(now=101.0) == UsageDisplay(seven_day_remaining=91)


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


def test_invalid_sparse_field_clears_that_slot_without_losing_the_other_window():
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
    assert updated.display_pair(now=126.0) == UsageDisplay(seven_day_remaining=91)
