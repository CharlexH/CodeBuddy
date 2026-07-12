from __future__ import annotations

import os
import subprocess
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import pytest

from codex_buddy import runtime
from codex_buddy.ota_trust import (
    bootstrap_ota_trust,
    export_public_trust,
    generate_ota_trust,
    load_ota_trust_pins,
)


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


def test_key_generation_does_not_implicitly_create_build_pins(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")

    assert not trust.trust_pins.is_file()
    with pytest.raises(RuntimeError, match="pin metadata"):
        load_ota_trust_pins(trust)


def test_explicit_bootstrap_atomically_pins_existing_trust_and_is_idempotent(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    original_keys = (
        trust.local_ca_private_key.read_bytes(),
        trust.manifest_private_key.read_bytes(),
    )

    bootstrapped = bootstrap_ota_trust(trust.root)
    pins = load_ota_trust_pins(bootstrapped)
    original_pin_bytes = trust.trust_pins.read_bytes()
    bootstrapped_again = bootstrap_ota_trust(trust.root)

    assert bootstrapped_again == trust
    assert trust.trust_pins.read_bytes() == original_pin_bytes
    assert _mode(trust.trust_pins) == 0o600
    assert _mode(trust.public_dir) == 0o700
    assert len(pins.ca_der_sha256) == 64
    assert len(pins.manifest_public_der_sha256) == 64
    assert (
        trust.local_ca_private_key.read_bytes(),
        trust.manifest_private_key.read_bytes(),
    ) == original_keys


def test_pin_rotation_requires_explicit_flag(tmp_path):
    trust = bootstrap_ota_trust(tmp_path / "ota")
    original_pins = trust.trust_pins.read_bytes()
    original_public = (
        trust.local_ca_certificate.read_bytes(),
        trust.manifest_public_key.read_bytes(),
    )
    other = generate_ota_trust(tmp_path / "other")
    trust.local_ca_private_key.write_bytes(other.local_ca_private_key.read_bytes())
    trust.manifest_private_key.write_bytes(other.manifest_private_key.read_bytes())
    trust.local_ca_private_key.chmod(0o600)
    trust.manifest_private_key.chmod(0o600)

    with pytest.raises(RuntimeError, match="rotation"):
        bootstrap_ota_trust(trust.root)
    assert trust.trust_pins.read_bytes() == original_pins
    assert (
        trust.local_ca_certificate.read_bytes(),
        trust.manifest_public_key.read_bytes(),
    ) == original_public

    rotated = bootstrap_ota_trust(trust.root, rotate_pins=True)

    assert rotated.trust_pins.read_bytes() != original_pins
    load_ota_trust_pins(rotated)


def test_pinned_bootstrap_never_regenerates_a_missing_private_key_implicitly(tmp_path):
    trust = bootstrap_ota_trust(tmp_path / "ota")
    trust.manifest_private_key.unlink()

    with pytest.raises(RuntimeError, match="rotation"):
        bootstrap_ota_trust(trust.root)

    assert not trust.manifest_private_key.exists()


def test_pin_loader_rejects_missing_corrupt_symlink_or_permissive_metadata(tmp_path):
    trust = bootstrap_ota_trust(tmp_path / "ota")
    original = trust.trust_pins.read_bytes()

    trust.trust_pins.unlink()
    with pytest.raises(RuntimeError, match="pin metadata"):
        load_ota_trust_pins(trust)
    trust.trust_pins.write_text("not json")
    trust.trust_pins.chmod(0o600)
    with pytest.raises(RuntimeError, match="pin metadata"):
        load_ota_trust_pins(trust)
    trust.trust_pins.write_bytes(original)
    trust.trust_pins.chmod(0o644)
    with pytest.raises(RuntimeError, match="0600"):
        load_ota_trust_pins(trust)
    trust.trust_pins.unlink()
    victim = tmp_path / "victim.json"
    victim.write_bytes(original)
    trust.trust_pins.symlink_to(victim)
    with pytest.raises(RuntimeError, match="non-symlink"):
        load_ota_trust_pins(trust)


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


def test_generation_atomically_repairs_legacy_ca_extensions_without_rotating_key(
    tmp_path, monkeypatch
):
    trust = generate_ota_trust(tmp_path / "ota")
    original_private_key = trust.local_ca_private_key.read_bytes()
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-new",
            "-sha256",
            "-key",
            str(trust.local_ca_private_key),
            "-out",
            str(trust.local_ca_certificate),
            "-days",
            "3650",
            "-subj",
            "/CN=Code Buddy Local OTA CA",
            "-addext",
            "basicConstraints=critical,CA:TRUE",
            "-addext",
            "keyUsage=critical,keyCertSign,cRLSign",
        ],
        check=True,
        capture_output=True,
    )
    legacy_certificate = trust.local_ca_certificate.read_bytes()
    replacements = []
    real_replace = os.replace

    def observing_replace(source, destination):
        replacements.append(Path(destination))
        return real_replace(source, destination)

    monkeypatch.setattr(os, "replace", observing_replace)

    repaired = generate_ota_trust(trust.root)

    assert repaired.local_ca_private_key.read_bytes() == original_private_key
    assert repaired.local_ca_certificate.read_bytes() != legacy_certificate
    assert trust.local_ca_certificate in replacements
    details = subprocess.run(
        ["openssl", "x509", "-in", str(repaired.local_ca_certificate), "-noout", "-text"],
        check=True,
        capture_output=True,
    ).stdout
    assert b"Subject Key Identifier" in details
    assert b"Authority Key Identifier" in details


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
