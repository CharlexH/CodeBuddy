from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Optional

from .ota_release import _parse_semantic_version, sign_manifest


_NONCE = re.compile(r"^[0-9A-Za-z_-]{24,48}$")
_DEVICE_NAME = re.compile(r"^Codex-[0-9A-F]{4}$")
_AUTHORIZATION_ACTION = "code-buddy-firmware-install-v1"
_AUTHORIZATION_MAX_LIFETIME_SECONDS = 300
_UINT32_MAX = 0xFFFFFFFF
_PHASES = {
    "running",
    "offer-received",
    "await-confirm",
    "accepted",
    "rejected",
    "busy",
    "authenticated",
    "download",
    "readback",
    "boot-committed",
    "restarting",
    "boot-health",
    "cancelled",
    "error",
}
_HEALTH = {"", "ordinary", "monitoring", "valid", "rollback", "error"}
_ERRORS = {
    "",
    "busy",
    "cancelled",
    "conflict",
    "download",
    "hash",
    "manifest",
    "power",
    "reconnect",
    "rejected",
    "rollback",
    "timeout",
    "trust",
    "version",
    "wifi",
}
_STATUS_KEYS = {
    "cmd",
    "nonce",
    "generation",
    "phase",
    "percent",
    "version",
    "health",
    "error",
    "cancel_applied",
}


class OtaProtocolError(ValueError):
    pass


@dataclass(frozen=True)
class OtaDeviceStatus:
    nonce: str
    generation: int
    phase: str
    percent: int
    version: str
    health: str
    error: str
    cancel_applied: bool


def valid_ota_nonce(nonce: object) -> bool:
    return isinstance(nonce, str) and _NONCE.fullmatch(nonce) is not None


def valid_ota_device_name(device: object) -> bool:
    return isinstance(device, str) and _DEVICE_NAME.fullmatch(device) is not None


def _validated_offer_payload(
    *,
    nonce: str,
    generation: int,
    version: str,
    size_bytes: int,
    manifest_url: str,
    signature_url: str,
) -> dict[str, object]:
    if not valid_ota_nonce(nonce):
        raise OtaProtocolError("invalid OTA nonce")
    if not isinstance(generation, int) or isinstance(generation, bool) or not 1 <= generation <= 0xFFFFFFFF:
        raise OtaProtocolError("invalid OTA generation")
    try:
        _parse_semantic_version(version)
    except ValueError as exc:
        raise OtaProtocolError("invalid OTA version") from exc
    if not isinstance(size_bytes, int) or isinstance(size_bytes, bool) or not 1 <= size_bytes <= 0x330000:
        raise OtaProtocolError("invalid OTA image size")
    for value, suffix in ((manifest_url, "/manifest.json"), (signature_url, "/manifest.sig")):
        if (
            not isinstance(value, str)
            or len(value.encode("ascii", errors="ignore")) != len(value)
            or len(value) > 255
            or not value.startswith("https://")
            or not value.endswith(suffix)
        ):
            raise OtaProtocolError("invalid OTA endpoint")
    return {
        "nonce": nonce,
        "generation": generation,
        "version": version,
        "sizeBytes": size_bytes,
        "manifestUrl": manifest_url,
        "signatureUrl": signature_url,
    }


def build_ota_offer(
    *,
    nonce: str,
    generation: int,
    version: str,
    size_bytes: int,
    manifest_url: str,
    signature_url: str,
) -> dict[str, object]:
    return {
        "ota_offer": _validated_offer_payload(
            nonce=nonce,
            generation=generation,
            version=version,
            size_bytes=size_bytes,
            manifest_url=manifest_url,
            signature_url=signature_url,
        )
    }


def canonical_ota_authorization_bytes(
    *,
    device: str,
    issued_at: int,
    expires_at: int,
    nonce: str,
    generation: int,
    version: str,
    size_bytes: int,
    manifest_url: str,
    signature_url: str,
) -> bytes:
    if not valid_ota_device_name(device):
        raise OtaProtocolError("invalid OTA device name")
    if (
        not isinstance(issued_at, int)
        or isinstance(issued_at, bool)
        or not isinstance(expires_at, int)
        or isinstance(expires_at, bool)
        or not 0 <= issued_at <= _UINT32_MAX
        or not issued_at < expires_at <= _UINT32_MAX
        or expires_at - issued_at > _AUTHORIZATION_MAX_LIFETIME_SECONDS
    ):
        raise OtaProtocolError("invalid OTA authorization time window")
    payload = _validated_offer_payload(
        nonce=nonce,
        generation=generation,
        version=version,
        size_bytes=size_bytes,
        manifest_url=manifest_url,
        signature_url=signature_url,
    )
    authorization = {
        "action": _AUTHORIZATION_ACTION,
        "device": device,
        "expiresAt": expires_at,
        "generation": payload["generation"],
        "issuedAt": issued_at,
        "manifestUrl": payload["manifestUrl"],
        "nonce": payload["nonce"],
        "signatureUrl": payload["signatureUrl"],
        "sizeBytes": payload["sizeBytes"],
        "version": payload["version"],
    }
    return json.dumps(
        authorization,
        ensure_ascii=True,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("ascii")


def build_signed_ota_offer(
    *,
    device: str,
    issued_at: int,
    expires_at: int,
    nonce: str,
    generation: int,
    version: str,
    size_bytes: int,
    manifest_url: str,
    signature_url: str,
    signing_private_key: Path,
) -> dict[str, object]:
    canonical = canonical_ota_authorization_bytes(
        device=device,
        issued_at=issued_at,
        expires_at=expires_at,
        nonce=nonce,
        generation=generation,
        version=version,
        size_bytes=size_bytes,
        manifest_url=manifest_url,
        signature_url=signature_url,
    )
    payload = _validated_offer_payload(
        nonce=nonce,
        generation=generation,
        version=version,
        size_bytes=size_bytes,
        manifest_url=manifest_url,
        signature_url=signature_url,
    )
    payload.update(
        {
            "device": device,
            "issuedAt": issued_at,
            "expiresAt": expires_at,
            "authorization": sign_manifest(
                canonical, Path(signing_private_key)
            ).hex(),
        }
    )
    return {"ota_offer": payload}


def parse_ota_status(
    payload: Mapping[str, object], *, nonce: str, generation: int
) -> OtaDeviceStatus:
    if not isinstance(payload, Mapping) or set(payload) != _STATUS_KEYS:
        raise OtaProtocolError("OTA status contains unexpected fields")
    if payload.get("cmd") != "ota_status":
        raise OtaProtocolError("not an OTA status event")
    payload_generation = payload.get("generation")
    if (
        payload.get("nonce") != nonce
        or not valid_ota_nonce(payload.get("nonce"))
        or not isinstance(payload_generation, int)
        or isinstance(payload_generation, bool)
        or payload_generation != generation
    ):
        raise OtaProtocolError("foreign or stale OTA status event")
    phase = payload.get("phase")
    percent = payload.get("percent")
    version = payload.get("version")
    health = payload.get("health")
    error = payload.get("error")
    cancel_applied = payload.get("cancel_applied")
    if not isinstance(phase, str) or phase not in _PHASES:
        raise OtaProtocolError("invalid OTA phase")
    if not isinstance(percent, int) or isinstance(percent, bool) or not 0 <= percent <= 100:
        raise OtaProtocolError("invalid OTA progress")
    if not isinstance(version, str) or len(version) > 63:
        raise OtaProtocolError("invalid OTA version")
    if version:
        try:
            _parse_semantic_version(version)
        except ValueError as exc:
            raise OtaProtocolError("invalid OTA version") from exc
    if not isinstance(health, str) or health not in _HEALTH:
        raise OtaProtocolError("invalid OTA boot health")
    if not isinstance(error, str) or error not in _ERRORS:
        raise OtaProtocolError("invalid OTA error")
    if not isinstance(cancel_applied, bool):
        raise OtaProtocolError("invalid OTA cancellation result")
    if cancel_applied and phase != "cancelled":
        raise OtaProtocolError("OTA cancellation result does not match phase")
    return OtaDeviceStatus(
        nonce=nonce,
        generation=generation,
        phase=phase,
        percent=percent,
        version=version,
        health=health,
        error=error,
        cancel_applied=cancel_applied,
    )
