from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Mapping, Optional

from .ota_release import _parse_semantic_version


_NONCE = re.compile(r"^[0-9A-Za-z_-]{24,48}$")
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


def build_ota_offer(
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
        "ota_offer": {
            "nonce": nonce,
            "generation": generation,
            "version": version,
            "sizeBytes": size_bytes,
            "manifestUrl": manifest_url,
            "signatureUrl": signature_url,
        }
    }


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
