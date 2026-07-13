import os
import hashlib
import struct
from pathlib import Path

import pytest

from codex_buddy import setup_flow
from codex_buddy.state_store import BridgeStateStore, PersistedState


def _application_image(version: str = "0.1.4") -> bytes:
    descriptor = bytearray(256)
    descriptor[:4] = struct.pack("<I", 0xABCD5432)
    descriptor[16 : 16 + len(version)] = version.encode("ascii")
    header = bytearray(24)
    header[0], header[1], header[2], header[3], header[23] = 0xE9, 2, 2, 0x3F, 1
    header[4:8] = struct.pack("<I", 0x40377B44)
    header[12:14] = struct.pack("<H", 9)
    image = bytes(header) + struct.pack("<II", 0x3C0E0020, len(descriptor)) + descriptor
    code = bytes(16)
    image += struct.pack("<II", 0x40377B40, len(code)) + code
    checksum = 0xEF
    for value in descriptor:
        checksum ^= value
    for value in code:
        checksum ^= value
    checksum_offset = (len(image) + 16) & ~15
    image += b"\0" * (checksum_offset - 1 - len(image)) + bytes([checksum])
    return image + hashlib.sha256(image).digest()


def test_migrate_legacy_state_moves_old_state_file(tmp_path):
    legacy_root = tmp_path / ".codex-buddy"
    runtime_root = tmp_path / ".code-buddy"
    legacy_root.mkdir()
    (legacy_root / "state.json").write_text('{"paired_device_id":"dev-1"}\n', encoding="utf-8")

    migrated = setup_flow.migrate_legacy_state(legacy_root=legacy_root, runtime_root=runtime_root)

    assert migrated is True
    assert not (legacy_root / "state.json").exists()
    assert (runtime_root / "state.json").read_text(encoding="utf-8") == '{"paired_device_id":"dev-1"}\n'


def test_migrate_legacy_state_restricts_existing_runtime_root(tmp_path):
    legacy_root = tmp_path / ".codex-buddy"
    runtime_root = tmp_path / ".code-buddy"
    runtime_root.mkdir(mode=0o755)

    assert setup_flow.migrate_legacy_state(
        legacy_root=legacy_root, runtime_root=runtime_root
    ) is False

    assert runtime_root.stat().st_mode & 0o777 == 0o700


def test_resolve_real_codex_path_skips_code_buddy_shim_dir(tmp_path, monkeypatch):
    shim_dir = tmp_path / ".code-buddy" / "bin"
    shim_dir.mkdir(parents=True)
    (shim_dir / "codex").write_text("#!/bin/sh\n", encoding="utf-8")

    real_dir = tmp_path / "usr" / "local" / "bin"
    real_dir.mkdir(parents=True)
    real_codex = real_dir / "codex"
    real_codex.write_text("#!/bin/sh\n", encoding="utf-8")
    real_codex.chmod(0o755)

    monkeypatch.setenv("PATH", f"{shim_dir}{os.pathsep}{real_dir}")

    resolved = setup_flow.resolve_real_codex_path(shim_dir)

    assert resolved == real_codex


def test_write_codex_shim_creates_executable_python_wrapper(tmp_path):
    shim_path = tmp_path / "bin" / "codex"

    setup_flow.write_codex_shim(shim_path, python_executable="/opt/homebrew/bin/python3")

    text = shim_path.read_text(encoding="utf-8")
    assert text.startswith("#!/opt/homebrew/bin/python3\n")
    assert "from codex_buddy.shim import main" in text
    assert shim_path.stat().st_mode & 0o111


def test_is_setup_complete_requires_metadata_and_runtime_files(tmp_path):
    state_path = tmp_path / "state.json"
    shim_dir = tmp_path / "bin"
    shim_dir.mkdir()
    shim_path = shim_dir / "codex"
    shim_path.write_text("#!/bin/sh\n", encoding="utf-8")
    shim_path.chmod(0o755)
    helper_path = tmp_path / "helper" / "CodeBuddyBLEHelper.app"
    helper_path.mkdir(parents=True)

    store = BridgeStateStore(state_path)
    store.save(
        PersistedState(
            paired_device_id="dev-1",
            setup_version=1,
            real_codex_path="/usr/local/bin/codex",
            helper_app_path=str(helper_path),
            shim_dir=str(shim_dir),
            shell_integrated=True,
            service_installed=True,
        )
    )

    assert setup_flow.is_setup_complete(store.load()) is True


def test_is_setup_complete_rejects_missing_required_state():
    assert setup_flow.is_setup_complete(PersistedState()) is False


def test_install_firmware_artifact_copies_release_app_image_without_following_symlink(tmp_path):
    source = tmp_path / "dist" / "code-buddy-sticks3-app.bin"
    source.parent.mkdir()
    source.write_bytes(_application_image())
    destination = tmp_path / "runtime" / "firmware.bin"

    assert setup_flow.ensure_firmware_artifact_installed(source, destination) == destination
    assert destination.read_bytes() == source.read_bytes()
    assert destination.stat().st_mode & 0o022 == 0

    original = destination.read_bytes()
    source.unlink()
    source.symlink_to(tmp_path / "secret")
    with pytest.raises(ValueError, match="non-symlink"):
        setup_flow.ensure_firmware_artifact_installed(source, destination)
    assert destination.read_bytes() == original


def test_install_firmware_artifact_fails_closed_when_source_is_missing(tmp_path):
    destination = tmp_path / "runtime" / "firmware.bin"
    destination.parent.mkdir()
    destination.write_bytes(b"previous-known-good")

    with pytest.raises(FileNotFoundError, match="firmware"):
        setup_flow.ensure_firmware_artifact_installed(
            tmp_path / "missing-app.bin", destination
        )

    assert destination.read_bytes() == b"previous-known-good"


def test_install_firmware_artifact_reads_the_bundled_package_resource(tmp_path):
    destination = tmp_path / "runtime" / "firmware.bin"

    installed = setup_flow.ensure_firmware_artifact_installed(
        destination=destination
    )

    assert installed == destination
    assert destination.read_bytes() == setup_flow.bundled_firmware_resource().read_bytes()
    assert destination.stat().st_mode & 0o022 == 0


def _fake_helper_app(root: Path, marker: bytes) -> Path:
    app = root / "CodeBuddyBLEHelper.app"
    executable = app / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
    executable.parent.mkdir(parents=True)
    executable.write_bytes(marker)
    executable.chmod(0o755)
    (app / "Contents" / "Info.plist").write_text("plist", encoding="utf-8")
    return app


def test_helper_install_refreshes_existing_app_atomically(tmp_path, monkeypatch):
    destination = _fake_helper_app(tmp_path / "installed", b"old")
    source = _fake_helper_app(tmp_path / "built", b"new")
    monkeypatch.setattr(setup_flow, "build_bundled_native_helper", lambda: source)
    monkeypatch.setattr(setup_flow.subprocess, "run", lambda *args, **kwargs: None)

    assert setup_flow.ensure_helper_app_installed(destination) == destination

    assert (destination / "Contents" / "MacOS" / "CodeBuddyBLEHelper").read_bytes() == b"new"


def test_helper_install_failure_preserves_previous_known_good_app(tmp_path, monkeypatch):
    destination = _fake_helper_app(tmp_path / "installed", b"old")

    def fail():
        raise RuntimeError("swift build failed")

    monkeypatch.setattr(setup_flow, "build_bundled_native_helper", fail)

    with pytest.raises(RuntimeError, match="swift build failed"):
        setup_flow.ensure_helper_app_installed(destination)

    assert (destination / "Contents" / "MacOS" / "CodeBuddyBLEHelper").read_bytes() == b"old"


def test_helper_install_replace_failure_restores_previous_app(tmp_path, monkeypatch):
    destination = _fake_helper_app(tmp_path / "installed", b"old")
    source = _fake_helper_app(tmp_path / "built", b"new")
    monkeypatch.setattr(setup_flow, "build_bundled_native_helper", lambda: source)
    monkeypatch.setattr(setup_flow.subprocess, "run", lambda *args, **kwargs: None)
    real_replace = setup_flow.os.replace
    calls = 0

    def fail_staging_replace(source_path, destination_path):
        nonlocal calls
        calls += 1
        if calls == 2:
            raise OSError("simulated atomic install failure")
        return real_replace(source_path, destination_path)

    monkeypatch.setattr(setup_flow.os, "replace", fail_staging_replace)

    with pytest.raises(OSError, match="atomic install failure"):
        setup_flow.ensure_helper_app_installed(destination)

    assert (destination / "Contents" / "MacOS" / "CodeBuddyBLEHelper").read_bytes() == b"old"
