#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional, Sequence, Tuple


CA_NAME = "local-ca-cert.pem"
PUBLIC_KEY_NAME = "manifest-signing-public.pem"
SHA256 = re.compile(r"^[0-9a-f]{64}$")


def _run(arguments: Sequence[str], *, input_bytes: Optional[bytes] = None) -> bytes:
    try:
        return subprocess.run(
            ["openssl", *arguments],
            input=input_bytes,
            check=True,
            capture_output=True,
        ).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("invalid OTA public trust material") from exc


def _regular_file(path: Path) -> None:
    try:
        metadata = path.lstat()
    except FileNotFoundError as exc:
        raise RuntimeError("OTA public trust material is missing") from exc
    if path.is_symlink() or not stat.S_ISREG(metadata.st_mode):
        raise RuntimeError("OTA public trust must use regular non-symlink files")


def _normalized_material(public_dir: Path) -> Tuple[bytes, bytes, str, str]:
    if public_dir.is_symlink() or not public_dir.is_dir():
        raise RuntimeError("OTA public trust directory is missing or unsafe")
    ca = public_dir / CA_NAME
    public_key = public_dir / PUBLIC_KEY_NAME
    _regular_file(ca)
    _regular_file(public_key)
    for path in (ca, public_key):
        if b"PRIVATE KEY" in path.read_bytes():
            raise RuntimeError("private OTA material cannot be injected")

    ca_pem = _run(["x509", "-in", str(ca), "-outform", "PEM"])
    ca_der = _run(["x509", "-in", str(ca), "-outform", "DER"])
    ca_details = _run(["x509", "-in", str(ca), "-noout", "-text"])
    _run(["verify", "-CAfile", str(ca), str(ca)])
    if b"CA:TRUE" not in ca_details or b"prime256v1" not in ca_details:
        raise RuntimeError("OTA local CA must be a P-256 CA certificate")

    public_pem = _run(
        ["pkey", "-pubin", "-in", str(public_key), "-pubout", "-outform", "PEM"]
    )
    public_der = _run(
        ["pkey", "-pubin", "-in", str(public_key), "-pubout", "-outform", "DER"]
    )
    public_details = _run(
        ["pkey", "-pubin", "-in", str(public_key), "-text", "-noout"]
    )
    if b"prime256v1" not in public_details:
        raise RuntimeError("OTA manifest public key must use P-256")

    return (
        ca_pem,
        public_pem,
        hashlib.sha256(ca_der).hexdigest(),
        hashlib.sha256(public_der).hexdigest(),
    )


def _require_expected(actual: str, expected: str, label: str) -> None:
    if not SHA256.fullmatch(expected) or not hmac.compare_digest(actual, expected):
        raise RuntimeError(f"OTA {label} fingerprint mismatch")


def _header(ca_pem: bytes, public_pem: bytes, ca_hash: str, public_hash: str) -> bytes:
    try:
        ca_text = ca_pem.decode("ascii")
        public_text = public_pem.decode("ascii")
    except UnicodeDecodeError as exc:
        raise RuntimeError("OTA public trust PEM must be ASCII") from exc
    return (
        "#pragma once\n"
        "#define CODE_BUDDY_OTA_TRUST_GENERATED 1\n"
        f"static constexpr char CODE_BUDDY_OTA_CA_PEM[] = {json.dumps(ca_text)};\n"
        "static constexpr char CODE_BUDDY_OTA_MANIFEST_PUBLIC_KEY_PEM[] = "
        f"{json.dumps(public_text)};\n"
        f"static constexpr char CODE_BUDDY_OTA_CA_SHA256[] = \"{ca_hash}\";\n"
        "static constexpr char CODE_BUDDY_OTA_MANIFEST_PUBLIC_KEY_SHA256[] = "
        f"\"{public_hash}\";\n"
    ).encode("ascii")


def inject(
    *,
    public_dir: Path,
    output: Path,
    expected_ca_sha256: str,
    expected_public_sha256: str,
) -> None:
    output = Path(output)
    try:
        ca_pem, public_pem, ca_hash, public_hash = _normalized_material(Path(public_dir))
        _require_expected(ca_hash, expected_ca_sha256, "CA")
        _require_expected(public_hash, expected_public_sha256, "manifest public key")
        contents = _header(ca_pem, public_pem, ca_hash, public_hash)
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.is_symlink():
            raise RuntimeError("OTA generated header must not be a symlink")
        if output.exists() and output.read_bytes() == contents:
            return
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=f".{output.name}.", dir=output.parent
        )
        temporary = Path(temporary_name)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(contents)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, output)
        finally:
            temporary.unlink(missing_ok=True)
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def _managed_paths(root: Path, repository_root: Optional[Path] = None):
    if repository_root is None:
        repository_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(repository_root / "src"))
    from codex_buddy.ota_trust import load_ota_trust_pins, ota_trust_paths

    trust = ota_trust_paths(Path(root))
    pins = load_ota_trust_pins(trust)
    return trust, pins


def inject_managed(
    *, root: Path, output: Path, repository_root: Optional[Path] = None
) -> None:
    output = Path(output)
    try:
        trust, pins = _managed_paths(Path(root), repository_root)
        inject(
            public_dir=trust.public_dir,
            output=output,
            expected_ca_sha256=pins.ca_der_sha256,
            expected_public_sha256=pins.manifest_public_der_sha256,
        )
    except BaseException:
        output.unlink(missing_ok=True)
        raise


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Inject public OTA trust into firmware")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--public-dir", type=Path)
    source.add_argument("--managed-root", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-ca-sha256")
    parser.add_argument("--expected-public-sha256")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    arguments = _parser().parse_args(argv)
    try:
        if arguments.managed_root:
            if arguments.expected_ca_sha256 or arguments.expected_public_sha256:
                raise ValueError("managed OTA trust fingerprints must come from pin metadata")
            inject_managed(root=arguments.managed_root, output=arguments.output)
        else:
            if not arguments.expected_ca_sha256 or not arguments.expected_public_sha256:
                raise ValueError("explicit public trust requires both pinned fingerprints")
            inject(
                public_dir=arguments.public_dir,
                output=arguments.output,
                expected_ca_sha256=arguments.expected_ca_sha256,
                expected_public_sha256=arguments.expected_public_sha256,
            )
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"OTA trust injection failed: {exc}", file=sys.stderr)
        return 1
    return 0


def _platformio() -> None:
    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()  # type: ignore[name-defined]
    output = project_dir / "generated/ota-trust/ota_trust_generated.h"
    explicit_public = os.environ.get("CODE_BUDDY_OTA_PUBLIC_DIR")
    try:
        if explicit_public:
            public_dir = Path(explicit_public).expanduser()
            expected_ca = os.environ.get("CODE_BUDDY_OTA_EXPECTED_CA_SHA256", "")
            expected_public = os.environ.get(
                "CODE_BUDDY_OTA_EXPECTED_MANIFEST_PUBLIC_SHA256", ""
            )
            if not expected_ca or not expected_public:
                raise RuntimeError(
                    "explicit OTA public trust requires pinned CA and manifest key fingerprints"
                )
            inject(
                public_dir=public_dir,
                output=output,
                expected_ca_sha256=expected_ca,
                expected_public_sha256=expected_public,
            )
        else:
            managed_root = Path(
                os.environ.get("CODE_BUDDY_OTA_TRUST_ROOT", "~/.code-buddy/ota")
            ).expanduser()
            inject_managed(
                root=managed_root,
                output=output,
                repository_root=project_dir.parent,
            )
    except BaseException:
        output.unlink(missing_ok=True)
        raise


if __name__ == "__main__":
    raise SystemExit(main())
else:
    try:
        Import("env")  # type: ignore[name-defined]  # noqa: F821
    except NameError:
        pass
    else:
        _platformio()
