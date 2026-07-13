import os
from pathlib import Path

from codex_buddy import setup_flow
from codex_buddy.state_store import BridgeStateStore, PersistedState


def test_migrate_legacy_state_moves_old_state_file(tmp_path):
    legacy_root = tmp_path / ".codex-buddy"
    runtime_root = tmp_path / ".code-buddy"
    legacy_root.mkdir()
    (legacy_root / "state.json").write_text('{"paired_device_id":"dev-1"}\n', encoding="utf-8")

    migrated = setup_flow.migrate_legacy_state(legacy_root=legacy_root, runtime_root=runtime_root)

    assert migrated is True
    assert not (legacy_root / "state.json").exists()
    assert (runtime_root / "state.json").read_text(encoding="utf-8") == '{"paired_device_id":"dev-1"}\n'


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
    source.write_bytes(b"app-image")
    destination = tmp_path / "runtime" / "firmware.bin"

    assert setup_flow.ensure_firmware_artifact_installed(source, destination) == destination
    assert destination.read_bytes() == b"app-image"
    assert destination.stat().st_mode & 0o022 == 0

    source.unlink()
    source.symlink_to(tmp_path / "secret")
    assert setup_flow.ensure_firmware_artifact_installed(source, destination) == destination
    assert destination.read_bytes() == b"app-image"
