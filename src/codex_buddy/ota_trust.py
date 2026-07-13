from __future__ import annotations

import fcntl
import hashlib
import hmac
import json
import os
import re
import stat
import subprocess
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator, Optional, Sequence, Tuple

from . import runtime


@dataclass(frozen=True)
class OtaTrustPaths:
    root: Path
    private_dir: Path
    public_dir: Path
    local_ca_private_key: Path
    local_ca_certificate: Path
    manifest_private_key: Path
    manifest_public_key: Path
    trust_pins: Path


@dataclass(frozen=True)
class OtaTrustPins:
    ca_der_sha256: str
    manifest_public_der_sha256: str


_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_PINS_SCHEMA = 1


def ota_trust_paths(root: Optional[Path] = None) -> OtaTrustPaths:
    trust_root = Path(root) if root is not None else runtime.ota_dir()
    private_dir = trust_root / "private"
    public_dir = trust_root / "public"
    return OtaTrustPaths(
        root=trust_root,
        private_dir=private_dir,
        public_dir=public_dir,
        local_ca_private_key=private_dir / "local-ca-key.pem",
        local_ca_certificate=public_dir / "local-ca-cert.pem",
        manifest_private_key=private_dir / "manifest-signing-key.pem",
        manifest_public_key=public_dir / "manifest-signing-public.pem",
        trust_pins=public_dir / "trust-pins.json",
    )


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
            check=check,
            capture_output=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to create OTA trust material") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"openssl failed: {detail or 'unknown error'}") from exc


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
def _exclusive_file_lock(path: Path) -> Iterator[None]:
    flags = os.O_CREAT | os.O_RDWR
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags, 0o600)
    except OSError as exc:
        raise RuntimeError(f"cannot safely open OTA lock {path}") from exc
    try:
        os.fchmod(descriptor, 0o600)
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        fcntl.flock(descriptor, fcntl.LOCK_UN)
        os.close(descriptor)


def _atomic_openssl_output(
    destination: Path,
    arguments: Callable[[Path], Sequence[str]],
    mode: int,
) -> None:
    _require_real_directory(destination.parent, create=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        os.close(descriptor)
        descriptor = -1
        temporary.chmod(0o600)
        _run_openssl(arguments(temporary))
        if temporary.stat().st_size == 0:
            raise RuntimeError(f"openssl produced an empty file for {destination.name}")
        sync_descriptor = os.open(temporary, os.O_RDONLY)
        try:
            os.fsync(sync_descriptor)
        finally:
            os.close(sync_descriptor)
        temporary.chmod(mode)
        os.replace(temporary, destination)
        destination.chmod(mode)
        _fsync_directory(destination.parent)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def _atomic_write(destination: Path, contents: bytes, mode: int) -> None:
    _require_real_directory(destination.parent, create=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        temporary.chmod(0o600)
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


def _generate_p256_private_key(destination: Path) -> None:
    _atomic_openssl_output(
        destination,
        lambda output: [
            "ecparam",
            "-name",
            "prime256v1",
            "-genkey",
            "-noout",
            "-out",
            str(output),
        ],
        0o600,
    )


def _generate_local_ca_certificate(private_key: Path, destination: Path) -> None:
    _atomic_openssl_output(
        destination,
        lambda output: [
            "req",
            "-x509",
            "-new",
            "-sha256",
            "-key",
            str(private_key),
            "-out",
            str(output),
            "-days",
            "3650",
            "-subj",
            "/CN=Code Buddy Local OTA CA",
            "-addext",
            "basicConstraints=critical,CA:TRUE",
            "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
            "-addext",
            "subjectKeyIdentifier=hash",
            "-addext",
            "authorityKeyIdentifier=keyid:always",
        ],
        0o644,
    )


def _export_public_key(private_key: Path, destination: Path) -> None:
    _atomic_openssl_output(
        destination,
        lambda output: [
            "pkey",
            "-in",
            str(private_key),
            "-pubout",
            "-out",
            str(output),
        ],
        0o644,
    )


def _validate_p256_private_key(private_key: Path, label: str) -> bytes:
    if private_key.is_symlink() or not private_key.is_file():
        raise RuntimeError(f"{label} must be a regular private key file")
    private_key.chmod(0o600)
    try:
        details = _run_openssl(
            ["ec", "-in", str(private_key), "-text", "-noout"]
        )
    except RuntimeError as exc:
        raise RuntimeError(f"{label} is malformed or invalid") from exc
    output = details.stdout + details.stderr
    if b"ASN1 OID: prime256v1" not in output:
        raise RuntimeError(f"{label} must use P-256 (prime256v1)")
    try:
        return _run_openssl(
            ["pkey", "-in", str(private_key), "-pubout", "-outform", "DER"]
        ).stdout
    except RuntimeError as exc:
        raise RuntimeError(f"{label} cannot derive a public key") from exc


def _public_key_der(public_key: Path) -> bytes:
    return _run_openssl(
        ["pkey", "-pubin", "-in", str(public_key), "-outform", "DER"]
    ).stdout


def _certificate_der(certificate: Path) -> bytes:
    return _run_openssl(
        ["x509", "-in", str(certificate), "-outform", "DER"]
    ).stdout


def _pins_bytes(pins: OtaTrustPins) -> bytes:
    return (
        json.dumps(
            {
                "caDerSha256": pins.ca_der_sha256,
                "manifestPublicDerSha256": pins.manifest_public_der_sha256,
                "schema": _PINS_SCHEMA,
            },
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("ascii")


def _current_public_pins(paths: OtaTrustPaths) -> OtaTrustPins:
    try:
        return OtaTrustPins(
            ca_der_sha256=hashlib.sha256(
                _certificate_der(paths.local_ca_certificate)
            ).hexdigest(),
            manifest_public_der_sha256=hashlib.sha256(
                _public_key_der(paths.manifest_public_key)
            ).hexdigest(),
        )
    except RuntimeError as exc:
        raise RuntimeError("OTA public trust cannot be pinned") from exc


def _require_protected_trust_directory(path: Path, label: str) -> None:
    try:
        metadata = path.lstat()
    except FileNotFoundError as exc:
        raise RuntimeError(f"OTA {label} directory is missing") from exc
    if path.is_symlink() or not stat.S_ISDIR(metadata.st_mode):
        raise RuntimeError(f"OTA {label} directory must be a real directory")
    if stat.S_IMODE(metadata.st_mode) != 0o700:
        raise RuntimeError(f"OTA {label} directory permissions must be exactly 0700")


def load_ota_trust_pins(trust: OtaTrustPaths) -> OtaTrustPins:
    _require_protected_trust_directory(trust.root, "trust root")
    _require_protected_trust_directory(trust.public_dir, "public trust")
    path = trust.trust_pins
    try:
        metadata = path.lstat()
    except FileNotFoundError as exc:
        raise RuntimeError("OTA trust pin metadata is missing; run explicit trust bootstrap") from exc
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError("OTA trust pin metadata must be a regular non-symlink file")
    if stat.S_IMODE(metadata.st_mode) != 0o600:
        raise RuntimeError("OTA trust pin metadata permissions must be exactly 0600")
    raw = path.read_bytes()
    if not raw or len(raw) > 512 or b"\x00" in raw or raw.startswith(b"\xef\xbb\xbf"):
        raise RuntimeError("OTA trust pin metadata is malformed")
    try:
        parsed = json.loads(raw.decode("utf-8"))
        if (
            not isinstance(parsed, dict)
            or set(parsed) != {"caDerSha256", "manifestPublicDerSha256", "schema"}
            or parsed["schema"] != _PINS_SCHEMA
            or isinstance(parsed["schema"], bool)
            or not isinstance(parsed["caDerSha256"], str)
            or not isinstance(parsed["manifestPublicDerSha256"], str)
            or not _SHA256.fullmatch(parsed["caDerSha256"])
            or not _SHA256.fullmatch(parsed["manifestPublicDerSha256"])
        ):
            raise ValueError("invalid pin schema")
        pins = OtaTrustPins(
            ca_der_sha256=parsed["caDerSha256"],
            manifest_public_der_sha256=parsed["manifestPublicDerSha256"],
        )
    except (UnicodeDecodeError, ValueError, TypeError, KeyError, json.JSONDecodeError) as exc:
        raise RuntimeError("OTA trust pin metadata is malformed") from exc
    if not hmac.compare_digest(raw, _pins_bytes(pins)):
        raise RuntimeError("OTA trust pin metadata is not canonical")
    return pins


def require_existing_ota_trust(root: Optional[Path] = None) -> OtaTrustPaths:
    """Validate already-pinned OTA trust without creating or repairing files."""
    trust = ota_trust_paths(root)
    try:
        pins = load_ota_trust_pins(trust)
    except RuntimeError as exc:
        raise RuntimeError(
            "OTA trust is unavailable; run explicit trust bootstrap before firmware update"
        ) from exc
    _require_protected_trust_directory(trust.private_dir, "private trust")
    for path, label, mode in (
        (trust.local_ca_private_key, "local CA private key", 0o600),
        (trust.manifest_private_key, "manifest signing private key", 0o600),
        (trust.local_ca_certificate, "local CA certificate", 0o644),
        (trust.manifest_public_key, "manifest signing public key", 0o644),
    ):
        try:
            metadata = path.lstat()
        except FileNotFoundError as exc:
            raise RuntimeError(f"OTA {label} is missing; run explicit trust bootstrap") from exc
        if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
            raise RuntimeError(f"OTA {label} must be a regular non-symlink file")
        if stat.S_IMODE(metadata.st_mode) != mode:
            raise RuntimeError(f"OTA {label} permissions must be exactly {mode:04o}")
    try:
        ca_public_der = _validate_p256_private_key(
            trust.local_ca_private_key, "local CA private key"
        )
        manifest_private_public_der = _validate_p256_private_key(
            trust.manifest_private_key, "manifest signing private key"
        )
        manifest_public_der = _public_key_der(trust.manifest_public_key)
        certificate_der = _certificate_der(trust.local_ca_certificate)
    except RuntimeError as exc:
        raise RuntimeError("OTA trust does not match pinned firmware trust") from exc
    matches = (
        _certificate_is_valid_ca(trust.local_ca_certificate, ca_public_der)
        and hmac.compare_digest(manifest_private_public_der, manifest_public_der)
        and hmac.compare_digest(
            hashlib.sha256(certificate_der).hexdigest(), pins.ca_der_sha256
        )
        and hmac.compare_digest(
            hashlib.sha256(manifest_public_der).hexdigest(),
            pins.manifest_public_der_sha256,
        )
    )
    if not matches:
        raise RuntimeError("OTA trust does not match pinned firmware trust")
    return trust


def _certificate_is_valid_ca(certificate: Path, expected_public_der: bytes) -> bool:
    if certificate.is_symlink() or not certificate.is_file():
        return False
    try:
        public_pem = _run_openssl(
            ["x509", "-in", str(certificate), "-pubkey", "-noout"]
        ).stdout
        certificate_public_der = _run_openssl(
            ["pkey", "-pubin", "-outform", "DER"],
            input_bytes=public_pem,
        ).stdout
        details = _run_openssl(
            ["x509", "-in", str(certificate), "-noout", "-text"]
        ).stdout
        _run_openssl(["x509", "-in", str(certificate), "-checkend", "0", "-noout"])
        _run_openssl(
            ["verify", "-CAfile", str(certificate), str(certificate)]
        )
    except RuntimeError:
        return False
    return (
        certificate_public_der == expected_public_der
        and b"CA:TRUE" in details
        and b"Certificate Sign" in details
        and b"Subject Key Identifier" in details
        and b"Authority Key Identifier" in details
    )


def _public_key_matches(public_key: Path, expected_der: bytes) -> bool:
    if public_key.is_symlink() or not public_key.is_file():
        return False
    try:
        return _public_key_der(public_key) == expected_der
    except RuntimeError:
        return False


def generate_ota_trust(root: Optional[Path] = None) -> OtaTrustPaths:
    paths = ota_trust_paths(root)
    _require_real_directory(paths.root, create=True)
    with _exclusive_file_lock(paths.root / ".generate.lock"):
        for directory in (paths.private_dir, paths.public_dir):
            _require_real_directory(directory, create=True)

        if not os.path.lexists(paths.local_ca_private_key):
            _generate_p256_private_key(paths.local_ca_private_key)
        ca_public_der = _validate_p256_private_key(
            paths.local_ca_private_key,
            "local CA private key",
        )
        if not _certificate_is_valid_ca(paths.local_ca_certificate, ca_public_der):
            _generate_local_ca_certificate(
                paths.local_ca_private_key,
                paths.local_ca_certificate,
            )

        if not os.path.lexists(paths.manifest_private_key):
            _generate_p256_private_key(paths.manifest_private_key)
        manifest_public_der = _validate_p256_private_key(
            paths.manifest_private_key,
            "manifest signing private key",
        )
        if not _public_key_matches(paths.manifest_public_key, manifest_public_der):
            _export_public_key(paths.manifest_private_key, paths.manifest_public_key)

        paths.local_ca_certificate.chmod(0o644)
        paths.manifest_public_key.chmod(0o644)
        return paths


def bootstrap_ota_trust(
    root: Optional[Path] = None,
    *,
    rotate_pins: bool = False,
) -> OtaTrustPaths:
    """Explicitly create/repair key material and deliberately establish build pins."""
    candidate_paths = ota_trust_paths(root)
    if os.path.lexists(candidate_paths.trust_pins) and not rotate_pins:
        # Once firmware trust is pinned, losing either private key is a key
        # rotation, not a repair. Never silently create a replacement.
        pinned_before = load_ota_trust_pins(candidate_paths)
        for private_key in (
            candidate_paths.local_ca_private_key,
            candidate_paths.manifest_private_key,
        ):
            if (
                not os.path.lexists(private_key)
                or private_key.is_symlink()
                or not private_key.is_file()
            ):
                raise RuntimeError(
                    "OTA private key rotation requires the explicit rotate-pins flag"
                )
        try:
            ca_public_der = _validate_p256_private_key(
                candidate_paths.local_ca_private_key,
                "local CA private key",
            )
            manifest_public_der = _validate_p256_private_key(
                candidate_paths.manifest_private_key,
                "manifest signing private key",
            )
            certificate_hash = hashlib.sha256(
                _certificate_der(candidate_paths.local_ca_certificate)
            ).hexdigest()
        except RuntimeError as exc:
            raise RuntimeError(
                "OTA trust key rotation requires the explicit rotate-pins flag"
            ) from exc
        if (
            not hmac.compare_digest(
                hashlib.sha256(manifest_public_der).hexdigest(),
                pinned_before.manifest_public_der_sha256,
            )
            or not hmac.compare_digest(
                certificate_hash,
                pinned_before.ca_der_sha256,
            )
            or not _certificate_is_valid_ca(
                candidate_paths.local_ca_certificate,
                ca_public_der,
            )
        ):
            raise RuntimeError(
                "OTA trust key rotation requires the explicit rotate-pins flag"
            )
    paths = generate_ota_trust(root)
    current = _current_public_pins(paths)
    with _exclusive_file_lock(paths.root / ".pins.lock"):
        if os.path.lexists(paths.trust_pins):
            try:
                pinned = load_ota_trust_pins(paths)
            except RuntimeError as exc:
                if not rotate_pins:
                    raise RuntimeError(
                        "OTA trust pins require explicit rotation to replace"
                    ) from exc
            else:
                matches = (
                    hmac.compare_digest(pinned.ca_der_sha256, current.ca_der_sha256)
                    and hmac.compare_digest(
                        pinned.manifest_public_der_sha256,
                        current.manifest_public_der_sha256,
                    )
                )
                if matches:
                    return paths
                if not rotate_pins:
                    raise RuntimeError(
                        "OTA trust key rotation requires the explicit rotate-pins flag"
                    )
        _atomic_write(paths.trust_pins, _pins_bytes(current), 0o600)
        return paths


def export_public_trust(
    trust: OtaTrustPaths,
    destination: Path,
) -> Tuple[Path, Path]:
    destination = Path(destination)
    _require_real_directory(destination, create=True)
    expected_names = {
        trust.local_ca_certificate.name,
        trust.manifest_public_key.name,
    }
    entries = list(destination.iterdir())
    symlinks = [path.name for path in entries if path.is_symlink()]
    if symlinks:
        raise ValueError(
            "public trust destination contains a symlink: "
            + ", ".join(sorted(symlinks))
        )
    unexpected = [path.name for path in entries if path.name not in expected_names]
    if unexpected:
        raise ValueError(
            "public trust destination contains unexpected material: "
            + ", ".join(sorted(unexpected))
        )

    exported = []
    for source in (trust.local_ca_certificate, trust.manifest_public_key):
        if source.is_symlink() or not source.is_file():
            raise ValueError("public OTA trust source must be a regular file")
        contents = source.read_bytes()
        if b"PRIVATE KEY" in contents:
            raise ValueError("refusing to export private OTA material")
        target = destination / source.name
        _atomic_write(target, contents, 0o644)
        exported.append(target)
    return exported[0], exported[1]
