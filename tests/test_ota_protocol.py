from __future__ import annotations

import json

import pytest

from codex_buddy.ota_protocol import (
    OtaProtocolError,
    build_ota_offer,
    build_signed_ota_offer,
    canonical_ota_authorization_bytes,
    parse_ota_status,
)
from codex_buddy.ota_release import verify_manifest_signature
from codex_buddy.ota_trust import generate_ota_trust


_MANIFEST_URL = "https://192.168.1.2:443/token-token-token-token-1234/manifest.json"
_SIGNATURE_URL = "https://192.168.1.2:443/token-token-token-token-1234/manifest.sig"


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


def test_authorization_bytes_are_canonical_and_domain_separated():
    arguments = dict(
        device="Codex-4DAD",
        issued_at=1_700_000_000,
        expires_at=1_700_000_120,
        nonce="n" * 24,
        generation=7,
        version="0.1.7",
        size_bytes=123456,
        manifest_url=_MANIFEST_URL,
        signature_url=_SIGNATURE_URL,
    )

    first = canonical_ota_authorization_bytes(**arguments)
    second = canonical_ota_authorization_bytes(**dict(reversed(tuple(arguments.items()))))

    assert first == second
    assert first == (
        b'{"action":"code-buddy-firmware-install-v1","device":"Codex-4DAD",'
        b'"expiresAt":1700000120,"generation":7,"issuedAt":1700000000,'
        b'"manifestUrl":"https://192.168.1.2:443/token-token-token-token-1234/manifest.json",'
        b'"nonce":"nnnnnnnnnnnnnnnnnnnnnnnn",'
        b'"signatureUrl":"https://192.168.1.2:443/token-token-token-token-1234/manifest.sig",'
        b'"sizeBytes":123456,"version":"0.1.7"}'
    )
    unsigned_fields = json.loads(first)
    unsigned_fields.pop("action")
    assert json.dumps(unsigned_fields, sort_keys=True, separators=(",", ":")).encode("ascii") != first


def test_signed_offer_verifies_and_has_strict_modern_shape(tmp_path):
    trust = generate_ota_trust(tmp_path / "trust")
    offer = build_signed_ota_offer(
        device="Codex-4DAD",
        issued_at=1_700_000_000,
        expires_at=1_700_000_120,
        nonce="n" * 24,
        generation=7,
        version="0.1.7",
        size_bytes=123456,
        manifest_url=_MANIFEST_URL,
        signature_url=_SIGNATURE_URL,
        signing_private_key=trust.manifest_private_key,
    )

    assert set(offer) == {"ota_offer"}
    payload = offer["ota_offer"]
    assert set(payload) == {
        "nonce", "generation", "version", "sizeBytes", "manifestUrl",
        "signatureUrl", "device", "issuedAt", "expiresAt", "authorization",
    }
    assert payload["authorization"] == payload["authorization"].lower()
    assert bytes.fromhex(payload["authorization"])
    canonical = canonical_ota_authorization_bytes(
        device=payload["device"],
        issued_at=payload["issuedAt"],
        expires_at=payload["expiresAt"],
        nonce=payload["nonce"],
        generation=payload["generation"],
        version=payload["version"],
        size_bytes=payload["sizeBytes"],
        manifest_url=payload["manifestUrl"],
        signature_url=payload["signatureUrl"],
    )
    assert verify_manifest_signature(
        canonical, bytes.fromhex(payload["authorization"]), trust.manifest_public_key
    )


@pytest.mark.parametrize("device", ["Codex-4dad", "Codex-12345", "Other-4DAD", ""])
def test_signed_offer_rejects_noncanonical_device_names(tmp_path, device):
    trust = generate_ota_trust(tmp_path / device.replace("/", "_") or "empty")
    with pytest.raises(OtaProtocolError, match="device"):
        build_signed_ota_offer(
            device=device,
            issued_at=1_700_000_000,
            expires_at=1_700_000_120,
            nonce="n" * 24,
            generation=7,
            version="0.1.7",
            size_bytes=123456,
            manifest_url=_MANIFEST_URL,
            signature_url=_SIGNATURE_URL,
            signing_private_key=trust.manifest_private_key,
        )


@pytest.mark.parametrize(
    ("issued_at", "expires_at"),
    [
        (1_700_000_000, 1_700_000_000),
        (1_700_000_001, 1_700_000_000),
        (1_700_000_000, 1_700_000_301),
        (True, 1_700_000_120),
        (-1, 120),
        (0, 0x1_0000_0000),
    ],
)
def test_signed_offer_rejects_invalid_or_overlong_time_windows(
    tmp_path, issued_at, expires_at
):
    trust = generate_ota_trust(tmp_path / f"trust-{issued_at}-{expires_at}")
    with pytest.raises(OtaProtocolError, match="time"):
        build_signed_ota_offer(
            device="Codex-4DAD",
            issued_at=issued_at,
            expires_at=expires_at,
            nonce="n" * 24,
            generation=7,
            version="0.1.7",
            size_bytes=123456,
            manifest_url=_MANIFEST_URL,
            signature_url=_SIGNATURE_URL,
            signing_private_key=trust.manifest_private_key,
        )


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
