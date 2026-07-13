from __future__ import annotations

import pytest

from codex_buddy.ota_protocol import (
    OtaProtocolError,
    build_ota_offer,
    parse_ota_status,
)


def test_offer_is_bounded_and_contains_only_device_hint_fields():
    offer = build_ota_offer(
        nonce="n" * 24,
        generation=7,
        version="0.1.5",
        size_bytes=123456,
        manifest_url="https://192.168.1.2:443/token-token-token-token-1234/manifest.json",
        signature_url="https://192.168.1.2:443/token-token-token-token-1234/manifest.sig",
    )

    assert set(offer) == {"ota_offer"}
    assert set(offer["ota_offer"]) == {
        "nonce",
        "generation",
        "version",
        "sizeBytes",
        "manifestUrl",
        "signatureUrl",
    }
    assert "firmware.bin" not in str(offer)


def test_status_parser_rejects_stale_or_secret_bearing_events():
    valid = {
        "cmd": "ota_status",
        "nonce": "n" * 24,
        "generation": 3,
        "phase": "download",
        "percent": 40,
        "version": "0.1.5",
        "health": "monitoring",
        "error": "",
        "cancel_applied": False,
    }
    assert parse_ota_status(valid, nonce="n" * 24, generation=3).percent == 40

    with pytest.raises(OtaProtocolError, match="foreign"):
        parse_ota_status(valid, nonce="x" * 24, generation=3)
    with pytest.raises(OtaProtocolError, match="foreign"):
        parse_ota_status(valid, nonce="n" * 24, generation=4)
    for forbidden in ("url", "token", "signature", "wifi", "password"):
        poisoned = dict(valid)
        poisoned[forbidden] = "secret"
        with pytest.raises(OtaProtocolError, match="unexpected"):
            parse_ota_status(poisoned, nonce="n" * 24, generation=3)


def test_status_parser_bounds_phase_progress_and_sanitized_error():
    base = {
        "cmd": "ota_status",
        "nonce": "n" * 24,
        "generation": 1,
        "phase": "error",
        "percent": 0,
        "version": "0.1.5",
        "health": "valid",
        "error": "download",
        "cancel_applied": False,
    }
    assert parse_ota_status(base, nonce="n" * 24, generation=1).error == "download"
    for key, value in (
        ("phase", "x" * 80),
        ("percent", 101),
        ("error", "https://192.168.1.2/secret"),
        ("cancel_applied", 1),
    ):
        invalid = dict(base)
        invalid[key] = value
        with pytest.raises(OtaProtocolError):
            parse_ota_status(invalid, nonce="n" * 24, generation=1)

    cancelled = dict(
        base,
        phase="cancelled",
        error="cancelled",
        cancel_applied=True,
    )
    assert parse_ota_status(
        cancelled, nonce="n" * 24, generation=1
    ).cancel_applied is True
