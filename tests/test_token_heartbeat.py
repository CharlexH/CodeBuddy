import base64
import math

from codex_buddy.token_heartbeat import BIN_SECONDS, MAX_RAW_VALUE, TokenHeartbeat


def decode(encoded: str) -> bytes:
    return base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4))


def test_first_observation_establishes_a_quiet_session_baseline():
    heartbeat = TokenHeartbeat()

    assert heartbeat.available is False
    heartbeat.observe("session-a", 1_000, now=10.0)

    assert heartbeat.available is True
    assert decode(heartbeat.encoded(10.0)) == bytes(64)


def test_zero_placeholder_does_not_make_token_data_available():
    heartbeat = TokenHeartbeat()

    heartbeat.observe("session-a", 0, now=10.0)

    assert heartbeat.available is False


def test_positive_delta_is_smoothed_across_future_bins():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 100, now=10.0)
    heartbeat.observe("session-a", 105, now=10.0)

    first = decode(heartbeat.encoded(10.0))
    second = decode(heartbeat.encoded(10.0 + BIN_SECONDS))
    third = decode(heartbeat.encoded(10.0 + 2 * BIN_SECONDS))

    assert first[-1] == intensity(1)
    assert second[-2:] == bytes((intensity(1), intensity(3)))
    assert third[-3:] == bytes((intensity(1), intensity(3), intensity(1)))


def test_counter_decrease_rebaselines_without_emitting_a_sample():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 100, now=10.0)
    heartbeat.observe("session-a", 80, now=10.0)
    heartbeat.observe("session-a", 85, now=11.0)

    assert decode(heartbeat.encoded(11.0))[-1] == intensity(1)


def test_sessions_have_independent_baselines_and_concurrent_deltas_accumulate():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 100, now=10.0)
    heartbeat.observe("session-b", 2_000, now=10.0)
    heartbeat.observe("session-a", 105, now=10.0)
    heartbeat.observe("session-b", 2_010, now=10.0)

    assert decode(heartbeat.encoded(10.0))[-1] == intensity(3)


def test_retain_sessions_prunes_disappeared_baselines_without_clearing_samples():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", 500, now=10.0)
    heartbeat.observe("session-b", 1_000, now=10.0)
    sample_time = 10.0 + 2 * BIN_SECONDS
    existing_curve = heartbeat.encoded(sample_time)

    heartbeat.retain_sessions({"session-b"})

    assert heartbeat.encoded(sample_time) == existing_curve
    before_new_baseline = heartbeat.encoded(11.0)
    heartbeat.observe("session-a", 750, now=11.0)
    assert heartbeat.encoded(11.0) == before_new_baseline


def test_samples_age_out_after_twenty_seconds():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", 5, now=10.0)

    assert any(decode(heartbeat.encoded(10.0 + 19.0)))
    assert decode(heartbeat.encoded(10.0 + 20.0)) == bytes(64)


def test_smoothing_uses_20_60_20_weights_without_losing_tokens():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", 500, now=10.0)

    assert heartbeat._raw_values(10.0)[-1] == 100
    assert heartbeat._raw_values(10.0 + BIN_SECONDS)[-2:] == (100, 300)
    assert heartbeat._raw_values(10.0 + 2 * BIN_SECONDS)[-3:] == (100, 300, 100)
    assert sum(heartbeat._raw_values(10.0 + 2 * BIN_SECONDS)) == 500


def test_raw_bins_use_saturating_arithmetic():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", MAX_RAW_VALUE * 10, now=10.0)

    assert heartbeat._raw_values(10.0)[-1] == MAX_RAW_VALUE


def test_intensities_use_the_fixed_log1p_reference_scale():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", 500, now=10.0)

    samples = decode(heartbeat.encoded(10.0 + 2 * BIN_SECONDS))
    assert samples[-3:] == bytes((intensity(100), intensity(300), intensity(100)))

    ceiling = TokenHeartbeat()
    ceiling.observe("session-a", 0, now=10.0)
    ceiling.observe("session-a", 160_000, now=10.0)
    assert decode(ceiling.encoded(10.0))[-1] == 255


def test_encoding_is_unpadded_url_safe_base64_for_exactly_sixty_four_bytes():
    heartbeat = TokenHeartbeat()
    heartbeat.observe("session-a", 0, now=10.0)
    heartbeat.observe("session-a", 500, now=10.0)

    encoded = heartbeat.encoded(10.0 + 2 * BIN_SECONDS)

    assert len(encoded) == 86
    assert "=" not in encoded
    assert all(character.isalnum() or character in "-_" for character in encoded)
    assert decode(encoded) == bytes((*([0] * 61), intensity(100), intensity(300), intensity(100)))


def intensity(value: int) -> int:
    return round(min(1.0, math.log1p(value) / math.log1p(32_000)) * 255)
