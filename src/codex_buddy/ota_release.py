from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence, Tuple
from urllib.parse import unquote, urlsplit


_SEMANTIC_VERSION = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)
_LOWER_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_TOKEN = re.compile(r"^[0-9A-Za-z_-]{16,}$")


@dataclass(frozen=True)
class SemanticVersion:
    major: int
    minor: int
    patch: int
    prerelease: Tuple[str, ...]


@dataclass(frozen=True)
class OtaRelease:
    output_dir: Path
    firmware: Path
    manifest: Path
    signature: Path


def _parse_semantic_version(value: str) -> SemanticVersion:
    match = _SEMANTIC_VERSION.fullmatch(value)
    if not match:
        raise ValueError(f"invalid semantic version: {value!r}")
    prerelease = tuple(match.group(4).split(".")) if match.group(4) else ()
    for identifier in prerelease:
        if identifier.isdigit() and len(identifier) > 1 and identifier.startswith("0"):
            raise ValueError(f"invalid semantic version: {value!r}")
    return SemanticVersion(
        major=int(match.group(1)),
        minor=int(match.group(2)),
        patch=int(match.group(3)),
        prerelease=prerelease,
    )


def _compare_prerelease(left: Tuple[str, ...], right: Tuple[str, ...]) -> int:
    if not left and not right:
        return 0
    if not left:
        return 1
    if not right:
        return -1
    for left_identifier, right_identifier in zip(left, right):
        if left_identifier == right_identifier:
            continue
        left_numeric = left_identifier.isdigit()
        right_numeric = right_identifier.isdigit()
        if left_numeric and right_numeric:
            return 1 if int(left_identifier) > int(right_identifier) else -1
        if left_numeric != right_numeric:
            return -1 if left_numeric else 1
        return 1 if left_identifier > right_identifier else -1
    return (len(left) > len(right)) - (len(left) < len(right))


def compare_semantic_versions(left: str, right: str) -> int:
    left_version = _parse_semantic_version(left)
    right_version = _parse_semantic_version(right)
    left_core = (left_version.major, left_version.minor, left_version.patch)
    right_core = (right_version.major, right_version.minor, right_version.patch)
    if left_core != right_core:
        return (left_core > right_core) - (left_core < right_core)
    return _compare_prerelease(left_version.prerelease, right_version.prerelease)


def require_monotonic_version(candidate: str, current: str) -> str:
    if compare_semantic_versions(candidate, current) <= 0:
        raise ValueError(f"OTA version {candidate!r} must be newer than {current!r}")
    return candidate


def _validate_artifact_url(value: str) -> None:
    try:
        parsed = urlsplit(value)
        _ = parsed.port
    except ValueError as exc:
        raise ValueError("artifact URL is invalid") from exc
    if parsed.scheme != "https" or not parsed.hostname:
        raise ValueError("artifact URL must use HTTPS")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("artifact URL must not contain credentials")
    if parsed.fragment:
        raise ValueError("artifact URL must not contain a fragment")
    if parsed.query:
        raise ValueError("artifact URL must not contain a query")
    decoded_path = unquote(parsed.path)
    segments = decoded_path.split("/")
    if any(segment in (".", "..") for segment in segments):
        raise ValueError("artifact URL path must be normalized")
    if len(segments) < 3 or segments[-1] != "firmware.bin" or not _TOKEN.fullmatch(segments[-2]):
        raise ValueError("artifact URL must contain a one-time token and end in firmware.bin")


def canonical_manifest_bytes(
    *,
    version: str,
    chip: str,
    size_bytes: int,
    sha256: str,
    artifact_url: str,
) -> bytes:
    _parse_semantic_version(version)
    if chip != "esp32s3":
        raise ValueError("chip must be esp32s3")
    if not isinstance(size_bytes, int) or isinstance(size_bytes, bool) or size_bytes <= 0:
        raise ValueError("artifact size must be a positive integer")
    if not _LOWER_SHA256.fullmatch(sha256):
        raise ValueError("SHA-256 must be 64 lowercase hexadecimal characters")
    _validate_artifact_url(artifact_url)
    manifest = {
        "artifact": {
            "sha256": sha256,
            "sizeBytes": size_bytes,
            "url": artifact_url,
        },
        "chip": chip,
        "schema": 1,
        "version": version,
    }
    return json.dumps(
        manifest,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def _run_openssl(
    arguments: Sequence[str],
    *,
    input_bytes: Optional[bytes] = None,
    check: bool = True,
) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(
            ["openssl", *arguments],
            input=input_bytes,
            capture_output=True,
            check=check,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to sign OTA releases") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"openssl failed: {detail or 'unknown error'}") from exc


def sign_manifest(manifest_bytes: bytes, private_key: Path) -> bytes:
    if not isinstance(manifest_bytes, bytes) or not manifest_bytes:
        raise ValueError("manifest must be non-empty bytes")
    private_key = Path(private_key)
    completed = _run_openssl(
        ["dgst", "-sha256", "-sign", str(private_key)],
        input_bytes=manifest_bytes,
    )
    if not completed.stdout:
        raise RuntimeError("openssl produced an empty OTA signature")
    return completed.stdout


def verify_manifest_signature(
    manifest_bytes: bytes,
    signature: bytes,
    public_key: Path,
) -> bool:
    descriptor, temporary_name = tempfile.mkstemp(prefix="code-buddy-ota-signature-")
    signature_path = Path(temporary_name)
    try:
        os.write(descriptor, signature)
        os.close(descriptor)
        descriptor = -1
        signature_path.chmod(0o600)
        completed = _run_openssl(
            [
                "dgst",
                "-sha256",
                "-verify",
                str(Path(public_key)),
                "-signature",
                str(signature_path),
            ],
            input_bytes=manifest_bytes,
            check=False,
        )
        return completed.returncode == 0
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        signature_path.unlink(missing_ok=True)


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.resolve().relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def _atomic_write(destination: Path, contents: bytes, mode: int = 0o644) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        os.write(descriptor, contents)
        os.close(descriptor)
        descriptor = -1
        temporary.chmod(mode)
        os.replace(temporary, destination)
        destination.chmod(mode)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def build_ota_release(
    *,
    image_path: Path,
    output_dir: Path,
    version: str,
    current_version: str,
    chip: str,
    artifact_url: str,
    signing_private_key: Path,
) -> OtaRelease:
    image_path = Path(image_path)
    output_dir = Path(output_dir)
    signing_private_key = Path(signing_private_key)
    require_monotonic_version(version, current_version)
    if not image_path.is_file():
        raise ValueError(f"firmware image does not exist: {image_path}")
    if _is_within(output_dir, signing_private_key.parent):
        raise ValueError("release output must not be inside the private trust directory")

    allowed_names = {"firmware.bin", "manifest.json", "manifest.sig"}
    output_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    output_dir.chmod(0o700)
    unexpected = [path.name for path in output_dir.iterdir() if path.name not in allowed_names]
    if unexpected:
        raise ValueError(
            "release output contains unexpected files: " + ", ".join(sorted(unexpected))
        )

    image_bytes = image_path.read_bytes()
    if not image_bytes:
        raise ValueError("firmware image must not be empty")
    manifest_bytes = canonical_manifest_bytes(
        version=version,
        chip=chip,
        size_bytes=len(image_bytes),
        sha256=hashlib.sha256(image_bytes).hexdigest(),
        artifact_url=artifact_url,
    )
    signature_bytes = sign_manifest(manifest_bytes, signing_private_key)

    firmware = output_dir / "firmware.bin"
    manifest = output_dir / "manifest.json"
    signature = output_dir / "manifest.sig"
    _atomic_write(firmware, image_bytes)
    _atomic_write(manifest, manifest_bytes)
    _atomic_write(signature, signature_bytes)
    return OtaRelease(
        output_dir=output_dir,
        firmware=firmware,
        manifest=manifest,
        signature=signature,
    )
