from __future__ import annotations

import fcntl
import hashlib
import ipaddress
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import tempfile
import threading
import uuid
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, Optional, Sequence, Tuple
from urllib.parse import unquote, urlsplit


_SEMANTIC_VERSION = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?"
    r"(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$"
)
_LOWER_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_OTA_SLOT_CAPACITY_BYTES = 0x330000
_OTA_UINT32_MAX = 0xFFFFFFFF
_OTA_VERSION_MAX_BYTES = 63
_OTA_URL_MAX_BYTES = 255
_OTA_MANIFEST_MAX_BYTES = 1024
_TOKEN = re.compile(r"^[0-9A-Za-z_-]{24,127}$")
_RFC1918_NETWORKS = tuple(
    ipaddress.ip_network(network)
    for network in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)
_THREAD_LOCKS: Dict[str, threading.Lock] = {}
_THREAD_LOCKS_GUARD = threading.Lock()


@dataclass(frozen=True)
class SemanticVersion:
    major: int
    minor: int
    patch: int
    prerelease: Tuple[str, ...]


@dataclass(frozen=True)
class OtaRelease:
    # output_dir is the atomically swapped "current" pointer. Consumers must
    # serve the immutable generation/file paths returned by this instance.
    output_dir: Path
    generation_dir: Path
    firmware: Path
    manifest: Path
    signature: Path


def _parse_semantic_version(value: str) -> SemanticVersion:
    if not isinstance(value, str) or len(value.encode("utf-8")) > _OTA_VERSION_MAX_BYTES:
        raise ValueError(f"invalid semantic version: {value!r}")
    match = _SEMANTIC_VERSION.fullmatch(value)
    if not match:
        raise ValueError(f"invalid semantic version: {value!r}")
    prerelease = tuple(match.group(4).split(".")) if match.group(4) else ()
    for identifier in prerelease:
        if identifier.isdigit() and len(identifier) > 1 and identifier.startswith("0"):
            raise ValueError(f"invalid semantic version: {value!r}")
    core = tuple(int(match.group(index)) for index in range(1, 4))
    if any(component > _OTA_UINT32_MAX for component in core):
        raise ValueError(f"invalid semantic version: {value!r}")
    return SemanticVersion(
        major=core[0],
        minor=core[1],
        patch=core[2],
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
    if not isinstance(value, str) or not value or any(
        ord(character) < 0x21 or ord(character) > 0x7E for character in value
    ):
        raise ValueError("artifact URL must use canonical visible ASCII")
    if len(value.encode("utf-8")) > _OTA_URL_MAX_BYTES:
        raise ValueError("artifact URL exceeds the device byte limit")
    if "?" in value or "#" in value:
        raise ValueError("artifact URL must not contain query or fragment delimiters")
    try:
        parsed = urlsplit(value)
        _ = parsed.port
    except ValueError as exc:
        raise ValueError("artifact URL is invalid") from exc
    if parsed.scheme != "https" or not parsed.hostname:
        raise ValueError("artifact URL must use HTTPS")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("artifact URL must not contain credentials")
    try:
        address = ipaddress.ip_address(parsed.hostname)
    except ValueError as exc:
        raise ValueError("artifact URL must use an RFC1918 IPv4 address") from exc
    if (
        not isinstance(address, ipaddress.IPv4Address)
        or str(address) != parsed.hostname
        or not any(address in network for network in _RFC1918_NETWORKS)
    ):
        raise ValueError("artifact URL must use an RFC1918 IPv4 address")
    if parsed.port is None or parsed.port <= 0:
        raise ValueError("artifact URL must include an explicit valid port")
    if parsed.netloc != f"{parsed.hostname}:{parsed.port}":
        raise ValueError("artifact URL authority must use canonical IPv4 and port text")
    if parsed.fragment:
        raise ValueError("artifact URL must not contain a fragment")
    if parsed.query:
        raise ValueError("artifact URL must not contain a query")
    decoded_path = unquote(parsed.path)
    if decoded_path != parsed.path:
        raise ValueError("artifact URL path must not use percent encoding")
    segments = decoded_path.split("/")
    if any(segment in (".", "..") for segment in segments):
        raise ValueError("artifact URL path must be normalized")
    if (
        len(segments) != 3
        or segments[0] != ""
        or segments[-1] != "firmware.bin"
        or not _TOKEN.fullmatch(segments[-2])
    ):
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
    if size_bytes > _OTA_SLOT_CAPACITY_BYTES:
        raise ValueError("artifact size exceeds the firmware OTA slot")
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
    encoded = json.dumps(
        manifest,
        ensure_ascii=False,
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    if len(encoded) > _OTA_MANIFEST_MAX_BYTES:
        raise ValueError("canonical OTA manifest exceeds the device byte limit")
    return encoded


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
        _write_all(descriptor, signature)
        os.fsync(descriptor)
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


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_all(descriptor: int, contents: bytes) -> None:
    remaining = memoryview(contents)
    while remaining:
        try:
            written = os.write(descriptor, remaining)
        except InterruptedError:
            continue
        if written <= 0:
            raise OSError("write returned without making progress")
        remaining = remaining[written:]


def _require_real_directory(directory: Path, *, create: bool) -> None:
    if os.path.lexists(directory) and directory.is_symlink():
        raise ValueError(f"directory must not be a symlink: {directory}")
    if create:
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        metadata = directory.lstat()
    except FileNotFoundError as exc:
        raise ValueError(f"directory does not exist: {directory}") from exc
    if not stat.S_ISDIR(metadata.st_mode):
        raise ValueError(f"path must be a directory: {directory}")
    directory.chmod(0o700)


@contextmanager
def _exclusive_build_lock(path: Path) -> Iterator[None]:
    key = str(path.resolve())
    with _THREAD_LOCKS_GUARD:
        thread_lock = _THREAD_LOCKS.setdefault(key, threading.Lock())
    with thread_lock:
        flags = os.O_CREAT | os.O_RDWR
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            descriptor = os.open(path, flags, 0o600)
        except OSError as exc:
            raise RuntimeError(f"cannot safely open OTA release lock {path}") from exc
        try:
            os.fchmod(descriptor, 0o600)
            fcntl.flock(descriptor, fcntl.LOCK_EX)
            yield
        finally:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
            os.close(descriptor)


def _atomic_write(destination: Path, contents: bytes, mode: int = 0o644) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        _write_all(descriptor, contents)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        temporary.chmod(mode)
        os.replace(temporary, destination)
        destination.chmod(mode)
        _fsync_directory(destination.parent)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def _validate_esp32s3_application_image(contents: bytes) -> None:
    if len(contents) < 33:
        raise ValueError("firmware must be an ESP32-S3 application image")
    segment_count = contents[1]
    spi_mode = contents[2]
    speed = contents[3] & 0x0F
    flash_size = contents[3] >> 4
    chip_id = struct.unpack_from("<H", contents, 12)[0]
    hash_appended = contents[23]
    if (
        contents[0] != 0xE9
        or not 1 <= segment_count <= 16
        or spi_mode > 5
        or speed not in (0, 1, 2, 0x0F)
        or flash_size > 7
        or chip_id != 9
        or hash_appended not in (0, 1)
    ):
        raise ValueError("firmware must be an ESP32-S3 application image")

    offset = 24
    for _ in range(segment_count):
        if offset + 8 > len(contents):
            raise ValueError("firmware must be an ESP32-S3 application image")
        _, data_length = struct.unpack_from("<II", contents, offset)
        offset += 8
        if data_length == 0 or data_length > len(contents) - offset:
            raise ValueError("firmware must be an ESP32-S3 application image")
        offset += data_length
    minimum_trailer = 1 + (32 if hash_appended else 0)
    if len(contents) - offset < minimum_trailer:
        raise ValueError("firmware must be an ESP32-S3 application image")


def _validate_signing_pair(
    private_key: Path,
    expected_public_key: Path,
) -> bytes:
    if private_key.is_symlink() or not private_key.is_file():
        raise ValueError("signing private key must be a regular non-symlink file")
    if stat.S_IMODE(private_key.lstat().st_mode) != 0o600:
        raise ValueError("signing private key permissions must be exactly 0600")
    if expected_public_key.is_symlink() or not expected_public_key.is_file():
        raise ValueError("expected firmware public key must be a regular non-symlink file")
    expected_public_key_bytes = expected_public_key.read_bytes()

    try:
        private_details = _run_openssl(
            ["ec", "-in", str(private_key), "-text", "-noout"]
        ).stdout
        expected_details = _run_openssl(
            ["pkey", "-pubin", "-text", "-noout"],
            input_bytes=expected_public_key_bytes,
        ).stdout
        derived_public_der = _run_openssl(
            ["pkey", "-in", str(private_key), "-pubout", "-outform", "DER"]
        ).stdout
        expected_public_der = _run_openssl(
            ["pkey", "-pubin", "-outform", "DER"],
            input_bytes=expected_public_key_bytes,
        ).stdout
    except RuntimeError as exc:
        raise ValueError("signing keys must be valid P-256 keys") from exc
    if (
        b"ASN1 OID: prime256v1" not in private_details
        or b"ASN1 OID: prime256v1" not in expected_details
    ):
        raise ValueError("signing keys must use P-256 (prime256v1)")
    if derived_public_der != expected_public_der:
        raise ValueError("signing private key does not match expected firmware public key")
    return expected_public_key_bytes


def _publish_release_generation(
    *,
    output_dir: Path,
    version: str,
    image_bytes: bytes,
    manifest_bytes: bytes,
    signature_bytes: bytes,
    expected_public_key_bytes: bytes,
) -> OtaRelease:
    parent = output_dir.parent
    _require_real_directory(parent, create=True)
    if os.path.lexists(output_dir) and not output_dir.is_symlink():
        raise ValueError("release current publication path must be a symlink")

    generation_root = parent / f".{output_dir.name}.generations"
    _require_real_directory(generation_root, create=True)
    staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=generation_root))
    staging.chmod(0o700)
    pointer_temporary = parent / f".{output_dir.name}.current-{uuid.uuid4().hex}"
    try:
        firmware = staging / "firmware.bin"
        manifest = staging / "manifest.json"
        signature = staging / "manifest.sig"
        _atomic_write(firmware, image_bytes)
        _atomic_write(manifest, manifest_bytes)
        _atomic_write(signature, signature_bytes)

        verification_key = staging / ".verification-public.pem"
        _atomic_write(verification_key, expected_public_key_bytes, 0o600)
        try:
            if not verify_manifest_signature(
                manifest.read_bytes(),
                signature.read_bytes(),
                verification_key,
            ):
                raise RuntimeError("staged OTA manifest signature failed offline verification")
        finally:
            verification_key.unlink(missing_ok=True)
        if {path.name for path in staging.iterdir()} != {
            "firmware.bin",
            "manifest.json",
            "manifest.sig",
        }:
            raise RuntimeError("staged OTA release contains unexpected material")
        if firmware.read_bytes() != image_bytes or manifest.read_bytes() != manifest_bytes:
            raise RuntimeError("staged OTA release failed readback verification")
        _fsync_directory(staging)

        generation = generation_root / f"{version}-{uuid.uuid4().hex}"
        os.replace(staging, generation)
        _fsync_directory(generation_root)

        relative_generation = os.path.relpath(generation, parent)
        os.symlink(relative_generation, pointer_temporary)
        os.replace(pointer_temporary, output_dir)
        _fsync_directory(parent)
        return OtaRelease(
            output_dir=output_dir,
            generation_dir=generation,
            firmware=generation / "firmware.bin",
            manifest=generation / "manifest.json",
            signature=generation / "manifest.sig",
        )
    finally:
        pointer_temporary.unlink(missing_ok=True)
        if staging.exists():
            shutil.rmtree(staging)


def build_ota_release(
    *,
    image_path: Path,
    output_dir: Path,
    version: str,
    current_version: str,
    chip: str,
    artifact_url: str,
    signing_private_key: Path,
    expected_signing_public_key: Path,
) -> OtaRelease:
    image_path = Path(image_path)
    output_dir = Path(output_dir)
    signing_private_key = Path(signing_private_key)
    expected_signing_public_key = Path(expected_signing_public_key)
    require_monotonic_version(version, current_version)
    if image_path.is_symlink() or not image_path.is_file():
        raise ValueError(f"firmware image must be a regular non-symlink file: {image_path}")
    expected_public_key_bytes = _validate_signing_pair(
        signing_private_key,
        expected_signing_public_key,
    )
    image_is_signing_key = image_path.resolve() == signing_private_key.resolve()
    image_is_in_managed_private_dir = (
        signing_private_key.parent.name == "private"
        and _is_within(image_path, signing_private_key.parent)
    )
    if image_is_signing_key or image_is_in_managed_private_dir:
        raise ValueError("firmware image must not come from the private trust directory")
    if (
        signing_private_key.parent.name == "private"
        and _is_within(output_dir, signing_private_key.parent)
    ):
        raise ValueError("release output must not be inside the private trust directory")

    image_bytes = image_path.read_bytes()
    _validate_esp32s3_application_image(image_bytes)
    manifest_bytes = canonical_manifest_bytes(
        version=version,
        chip=chip,
        size_bytes=len(image_bytes),
        sha256=hashlib.sha256(image_bytes).hexdigest(),
        artifact_url=artifact_url,
    )
    signature_bytes = sign_manifest(manifest_bytes, signing_private_key)
    _require_real_directory(output_dir.parent, create=True)
    with _exclusive_build_lock(output_dir.parent / f".{output_dir.name}.build.lock"):
        return _publish_release_generation(
            output_dir=output_dir,
            version=version,
            image_bytes=image_bytes,
            manifest_bytes=manifest_bytes,
            signature_bytes=signature_bytes,
            expected_public_key_bytes=expected_public_key_bytes,
        )
