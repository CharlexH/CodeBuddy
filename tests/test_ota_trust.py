from __future__ import annotations

import os
import subprocess
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import pytest

from codex_buddy import runtime
from codex_buddy.ota_trust import export_public_trust, generate_ota_trust


def _mode(path: Path) -> int:
    return path.stat().st_mode & 0o777


def _generate_in_process(root: str) -> bytes:
    return generate_ota_trust(Path(root)).manifest_public_key.read_bytes()


def test_runtime_exposes_protected_ota_paths(monkeypatch, tmp_path):
    monkeypatch.setattr(runtime, "runtime_root", lambda: tmp_path / ".code-buddy")

    assert runtime.ota_dir() == tmp_path / ".code-buddy" / "ota"
    assert runtime.ota_private_dir() == runtime.ota_dir() / "private"
    assert runtime.ota_public_dir() == runtime.ota_dir() / "public"
    assert runtime.ota_releases_dir() == runtime.ota_dir() / "releases"


def test_generation_creates_two_independent_p256_key_pairs_with_safe_modes(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")

    assert _mode(trust.root) == 0o700
    assert _mode(trust.private_dir) == 0o700
    assert _mode(trust.public_dir) == 0o700
    assert _mode(trust.local_ca_private_key) == 0o600
    assert _mode(trust.manifest_private_key) == 0o600
    assert _mode(trust.local_ca_certificate) == 0o644
    assert _mode(trust.manifest_public_key) == 0o644

    ca_public = subprocess.run(
        ["openssl", "x509", "-in", str(trust.local_ca_certificate), "-pubkey", "-noout"],
        check=True,
        capture_output=True,
    ).stdout
    assert ca_public != trust.manifest_public_key.read_bytes()

    for private_key in (trust.local_ca_private_key, trust.manifest_private_key):
        details = subprocess.run(
            ["openssl", "ec", "-in", str(private_key), "-text", "-noout"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
        assert "ASN1 OID: prime256v1" in details


def test_generation_is_idempotent_and_repairs_private_permissions(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    original = {
        path: path.read_bytes()
        for path in (
            trust.local_ca_private_key,
            trust.local_ca_certificate,
            trust.manifest_private_key,
            trust.manifest_public_key,
        )
    }
    trust.manifest_private_key.chmod(0o644)

    regenerated = generate_ota_trust(tmp_path / "ota")

    assert regenerated == trust
    assert {path: path.read_bytes() for path in original} == original
    assert _mode(trust.manifest_private_key) == 0o600


def test_generation_handles_shell_metacharacters_without_shell_execution(tmp_path):
    injected = tmp_path / "ota with spaces; touch SHOULD_NOT_EXIST"

    trust = generate_ota_trust(injected)

    assert trust.local_ca_certificate.is_file()
    assert not (tmp_path / "SHOULD_NOT_EXIST").exists()


def test_public_export_contains_only_firmware_safe_material(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    destination = tmp_path / "firmware trust"

    exported = export_public_trust(trust, destination)

    assert {path.name for path in exported} == {
        "local-ca-cert.pem",
        "manifest-signing-public.pem",
    }
    assert not any("PRIVATE KEY" in path.read_text() for path in destination.iterdir())
    assert not any(path.name.endswith("key.pem") for path in destination.iterdir())
    assert all(_mode(path) == 0o644 for path in exported)


def test_private_key_files_are_created_without_world_readable_window(tmp_path, monkeypatch):
    observed_private_modes = []
    real_replace = os.replace

    def observing_replace(source, destination):
        if Path(destination).parent.name == "private":
            observed_private_modes.append(Path(source).stat().st_mode & 0o777)
        return real_replace(source, destination)

    monkeypatch.setattr(os, "replace", observing_replace)

    generate_ota_trust(tmp_path / "ota")

    assert observed_private_modes
    assert all(mode & 0o077 == 0 for mode in observed_private_modes)


def test_generation_repairs_mismatched_public_key_and_ca_certificate(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    other = generate_ota_trust(tmp_path / "other")
    expected_manifest_public = trust.manifest_public_key.read_bytes()
    expected_ca_public = subprocess.run(
        ["openssl", "x509", "-in", str(trust.local_ca_certificate), "-pubkey", "-noout"],
        check=True,
        capture_output=True,
    ).stdout
    trust.manifest_public_key.write_bytes(other.manifest_public_key.read_bytes())
    trust.local_ca_certificate.write_bytes(other.local_ca_certificate.read_bytes())

    repaired = generate_ota_trust(trust.root)

    assert repaired.manifest_public_key.read_bytes() == expected_manifest_public
    repaired_ca_public = subprocess.run(
        ["openssl", "x509", "-in", str(repaired.local_ca_certificate), "-pubkey", "-noout"],
        check=True,
        capture_output=True,
    ).stdout
    assert repaired_ca_public == expected_ca_public


def test_generation_repairs_corrupt_derived_material(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    trust.manifest_public_key.write_text("not a public key")
    trust.local_ca_certificate.write_text("not a certificate")

    repaired = generate_ota_trust(trust.root)

    subprocess.run(
        ["openssl", "pkey", "-pubin", "-in", str(repaired.manifest_public_key), "-noout"],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["openssl", "x509", "-in", str(repaired.local_ca_certificate), "-noout"],
        check=True,
        capture_output=True,
    )


def test_generation_fails_closed_on_malformed_or_wrong_curve_private_key(tmp_path):
    malformed_root = tmp_path / "malformed"
    malformed = generate_ota_trust(malformed_root)
    malformed.manifest_private_key.write_text("not a private key")

    with pytest.raises(RuntimeError, match="manifest signing private key"):
        generate_ota_trust(malformed_root)

    wrong_curve_root = tmp_path / "wrong-curve"
    wrong_curve = generate_ota_trust(wrong_curve_root)
    subprocess.run(
        [
            "openssl",
            "ecparam",
            "-name",
            "secp384r1",
            "-genkey",
            "-noout",
            "-out",
            str(wrong_curve.manifest_private_key),
        ],
        check=True,
    )

    with pytest.raises(RuntimeError, match="P-256"):
        generate_ota_trust(wrong_curve_root)


def test_cross_process_generation_is_serialized_and_idempotent(tmp_path):
    root = str(tmp_path / "ota")

    with ProcessPoolExecutor(max_workers=4) as executor:
        public_keys = list(executor.map(_generate_in_process, [root] * 8))

    assert len(set(public_keys)) == 1
    generate_ota_trust(Path(root))


def test_public_export_rejects_destination_directory_symlink(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    victim = tmp_path / "victim"
    victim.mkdir()
    destination = tmp_path / "public-export"
    destination.symlink_to(victim, target_is_directory=True)

    with pytest.raises(ValueError, match="symlink"):
        export_public_trust(trust, destination)

    assert list(victim.iterdir()) == []


def test_public_export_rejects_entry_symlink_without_touching_victim(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    destination = tmp_path / "public-export"
    destination.mkdir()
    victim = tmp_path / "victim.pem"
    victim.write_text("keep me")
    (destination / "local-ca-cert.pem").symlink_to(victim)

    with pytest.raises(ValueError, match="symlink"):
        export_public_trust(trust, destination)

    assert victim.read_text() == "keep me"
