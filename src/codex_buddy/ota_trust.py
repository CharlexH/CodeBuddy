from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Sequence, Tuple

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
    )


def _run_openssl(arguments: Sequence[str]) -> None:
    try:
        subprocess.run(
            ["openssl", *arguments],
            check=True,
            capture_output=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to create OTA trust material") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"openssl failed: {detail or 'unknown error'}") from exc


def _atomic_openssl_output(
    destination: Path,
    arguments: Callable[[Path], Sequence[str]],
    mode: int,
) -> None:
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    destination.parent.chmod(0o700)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.",
        dir=destination.parent,
    )
    temporary = Path(temporary_name)
    try:
        os.close(descriptor)
        temporary.chmod(0o600)
        _run_openssl(arguments(temporary))
        if temporary.stat().st_size == 0:
            raise RuntimeError(f"openssl produced an empty file for {destination.name}")
        temporary.chmod(mode)
        os.replace(temporary, destination)
        destination.chmod(mode)
    finally:
        try:
            os.close(descriptor)
        except OSError:
            pass
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


def generate_ota_trust(root: Optional[Path] = None) -> OtaTrustPaths:
    paths = ota_trust_paths(root)
    for directory in (paths.root, paths.private_dir, paths.public_dir):
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        directory.chmod(0o700)

    if not paths.local_ca_private_key.exists():
        _generate_p256_private_key(paths.local_ca_private_key)
        paths.local_ca_certificate.unlink(missing_ok=True)
    paths.local_ca_private_key.chmod(0o600)
    if not paths.local_ca_certificate.exists():
        _generate_local_ca_certificate(
            paths.local_ca_private_key,
            paths.local_ca_certificate,
        )

    if not paths.manifest_private_key.exists():
        _generate_p256_private_key(paths.manifest_private_key)
        paths.manifest_public_key.unlink(missing_ok=True)
    paths.manifest_private_key.chmod(0o600)
    if not paths.manifest_public_key.exists():
        _export_public_key(paths.manifest_private_key, paths.manifest_public_key)

    paths.local_ca_certificate.chmod(0o644)
    paths.manifest_public_key.chmod(0o644)
    return paths


def export_public_trust(
    trust: OtaTrustPaths,
    destination: Path,
) -> Tuple[Path, Path]:
    destination = Path(destination)
    destination.mkdir(mode=0o700, parents=True, exist_ok=True)
    destination.chmod(0o700)
    expected_names = {
        trust.local_ca_certificate.name,
        trust.manifest_public_key.name,
    }
    unexpected = [path.name for path in destination.iterdir() if path.name not in expected_names]
    if unexpected:
        raise ValueError(
            "public trust destination contains unexpected material: "
            + ", ".join(sorted(unexpected))
        )

    exported = []
    for source in (trust.local_ca_certificate, trust.manifest_public_key):
        if "PRIVATE KEY" in source.read_text(encoding="ascii"):
            raise ValueError("refusing to export private OTA material")
        target = destination / source.name
        shutil.copyfile(source, target)
        target.chmod(0o644)
        exported.append(target)
    return exported[0], exported[1]
