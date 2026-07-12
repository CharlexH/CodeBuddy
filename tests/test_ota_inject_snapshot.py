from __future__ import annotations

import hashlib
import importlib.util
import os
import subprocess
from pathlib import Path

import pytest

from codex_buddy.ota_trust import generate_ota_trust


PROJECT_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = PROJECT_ROOT / "firmware/scripts/inject-ota-trust.py"


def _load_inject_module():
    spec = importlib.util.spec_from_file_location("ota_inject_snapshot", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def inject_module():
    return _load_inject_module()


@pytest.fixture
def trust(tmp_path):
    return generate_ota_trust(tmp_path / "trust")


def _openssl(arguments: list[str], contents: bytes) -> bytes:
    return subprocess.run(
        ["openssl", *arguments],
        input=contents,
        check=True,
        capture_output=True,
    ).stdout


def test_public_files_are_opened_once_and_path_swaps_do_not_change_snapshot(
    inject_module, trust, tmp_path, monkeypatch
):
    ca_path = trust.local_ca_certificate
    public_path = trust.manifest_public_key
    original_ca = ca_path.read_bytes()
    original_public = public_path.read_bytes()
    replacement = generate_ota_trust(tmp_path / "replacement")
    replacement_bytes = {
        ca_path: replacement.local_ca_certificate.read_bytes(),
        public_path: replacement.manifest_public_key.read_bytes(),
    }
    original_open = inject_module.os.open
    open_counts = {ca_path: 0, public_path: 0}
    open_flags = {}

    def swapping_open(path, flags, *args, **kwargs):
        candidate = Path(path)
        descriptor = original_open(path, flags, *args, **kwargs)
        if candidate in open_counts:
            open_counts[candidate] += 1
            open_flags[candidate] = flags
            temporary = candidate.with_name(f".{candidate.name}.replacement")
            temporary.write_bytes(replacement_bytes[candidate])
            os.replace(temporary, candidate)
        return descriptor

    monkeypatch.setattr(inject_module.os, "open", swapping_open)

    ca_pem, public_pem, ca_hash, public_hash = inject_module._normalized_material(
        trust.public_dir
    )

    assert open_counts == {ca_path: 1, public_path: 1}
    assert all(flags & os.O_NOFOLLOW for flags in open_flags.values())
    assert ca_pem == _openssl(["x509", "-outform", "PEM"], original_ca)
    assert public_pem == _openssl(
        ["pkey", "-pubin", "-pubout", "-outform", "PEM"], original_public
    )
    assert ca_hash == hashlib.sha256(
        _openssl(["x509", "-outform", "DER"], original_ca)
    ).hexdigest()
    assert public_hash == hashlib.sha256(
        _openssl(
            ["pkey", "-pubin", "-pubout", "-outform", "DER"],
            original_public,
        )
    ).hexdigest()


def test_all_openssl_parsing_uses_the_captured_file_bytes(
    inject_module, trust, monkeypatch
):
    captured = {
        trust.local_ca_certificate.read_bytes(),
        trust.manifest_public_key.read_bytes(),
    }
    original_run = inject_module._run
    calls = []

    def recording_run(arguments, *, input_bytes=None):
        calls.append((tuple(arguments), input_bytes))
        return original_run(arguments, input_bytes=input_bytes)

    monkeypatch.setattr(inject_module, "_run", recording_run)

    inject_module._normalized_material(trust.public_dir)

    assert calls
    assert all(input_bytes in captured for _, input_bytes in calls)
    assert all(str(trust.public_dir) not in " ".join(arguments) for arguments, _ in calls)


def test_symlinked_public_file_is_rejected(inject_module, trust):
    public_key = trust.manifest_public_key
    target = public_key.with_name("real-public.pem")
    public_key.replace(target)
    public_key.symlink_to(target)

    with pytest.raises(RuntimeError, match="non-symlink|safely open"):
        inject_module._normalized_material(trust.public_dir)


def test_oversized_public_file_is_rejected_before_openssl(inject_module, trust):
    trust.manifest_public_key.write_bytes(b"A" * (64 * 1024 + 1))

    with pytest.raises(RuntimeError, match="too large"):
        inject_module._normalized_material(trust.public_dir)


@pytest.mark.parametrize("mode", [0o600, 0o644])
def test_explicit_private_and_managed_public_modes_are_accepted(
    inject_module, trust, mode
):
    trust.local_ca_certificate.chmod(mode)
    trust.manifest_public_key.chmod(mode)

    inject_module._normalized_material(trust.public_dir)


def test_group_writable_public_file_is_rejected(inject_module, trust):
    trust.manifest_public_key.chmod(0o664)

    with pytest.raises(RuntimeError, match="permissions"):
        inject_module._normalized_material(trust.public_dir)


def test_public_file_must_be_owned_by_effective_user(
    inject_module, trust, monkeypatch
):
    different_uid = os.geteuid() + 1
    monkeypatch.setattr(inject_module.os, "geteuid", lambda: different_uid)

    with pytest.raises(RuntimeError, match="owner"):
        inject_module._normalized_material(trust.public_dir)
