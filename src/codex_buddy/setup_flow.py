from __future__ import annotations

import os
import shutil
import tempfile
import subprocess
import uuid
from importlib import resources
from pathlib import Path

from . import runtime
from .native_helper_build import (
    build_bundled_native_helper,
    cleanup_bundled_native_helper_build,
)
from .ota_release import inspect_esp32s3_application_image
from .state_store import PersistedState

SETUP_VERSION = 1


def migrate_legacy_state(
    *,
    legacy_root: Path | None = None,
    runtime_root: Path | None = None,
) -> bool:
    legacy_root = runtime.legacy_runtime_root() if legacy_root is None else legacy_root
    runtime_root = runtime.runtime_root() if runtime_root is None else runtime_root
    legacy_state_path = legacy_root / "state.json"
    next_state_path = runtime_root / "state.json"
    if not legacy_state_path.exists() or next_state_path.exists():
        return False
    runtime_root.mkdir(parents=True, exist_ok=True)
    shutil.move(str(legacy_state_path), str(next_state_path))
    return True


def resolve_real_codex_path(shim_dir: Path, *, saved_path: str = "") -> Path:
    if saved_path:
        candidate = Path(saved_path).expanduser()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if not entry:
            continue
        base = Path(entry).expanduser()
        if base == shim_dir:
            continue
        candidate = base / "codex"
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise FileNotFoundError("Unable to locate a real `codex` executable in PATH")


def write_codex_shim(shim_path: Path, *, python_executable: str) -> None:
    shim_path.parent.mkdir(parents=True, exist_ok=True)
    shim_path.write_text(
        "\n".join(
            [
                f"#!{python_executable}",
                "from codex_buddy.shim import main",
                "",
                'if __name__ == "__main__":',
                "    raise SystemExit(main())",
                "",
            ]
        ),
        encoding="utf-8",
    )
    shim_path.chmod(0o755)


def ensure_helper_app_installed(destination: Path | None = None) -> Path:
    destination = runtime.helper_app_path() if destination is None else destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    source = build_bundled_native_helper()
    staging = destination.parent / f".{destination.name}.staging-{uuid.uuid4().hex}"
    backup = destination.parent / f".{destination.name}.backup-{uuid.uuid4().hex}"
    moved_old = False
    try:
        shutil.copytree(source, staging, symlinks=False)
        executable = staging / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
        if not executable.is_file() or executable.is_symlink() or not os.access(executable, os.X_OK):
            raise RuntimeError("built native BLE helper executable is invalid")
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", str(staging)],
            check=True,
            capture_output=True,
            text=True,
        )
        if destination.exists() or destination.is_symlink():
            os.replace(destination, backup)
            moved_old = True
        try:
            os.replace(staging, destination)
        except BaseException:
            if moved_old:
                os.replace(backup, destination)
                moved_old = False
            raise
        if moved_old:
            shutil.rmtree(backup)
        return destination
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
        if backup.exists() and destination.exists():
            shutil.rmtree(backup, ignore_errors=True)
        cleanup_bundled_native_helper_build(source)


def bundled_firmware_resource():
    return resources.files("codex_buddy").joinpath(
        "firmware", "code-buddy-sticks3-app.bin"
    )


def ensure_firmware_artifact_installed(
    source: Path | None = None, destination: Path | None = None
) -> Path:
    destination = runtime.default_firmware_path() if destination is None else Path(destination)
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        if source is None:
            resource = bundled_firmware_resource()
            try:
                source_handle = resource.open("rb")
            except FileNotFoundError as exc:
                raise FileNotFoundError(
                    "bundled firmware application image is missing from the Python package"
                ) from exc
        else:
            source = Path(source)
            if source.is_symlink():
                raise ValueError(
                    f"firmware image must be a regular non-symlink file: {source}"
                )
            if not source.is_file():
                raise FileNotFoundError(f"firmware application image is missing: {source}")
            source_handle = source.open("rb")
        with source_handle:
            with os.fdopen(descriptor, "wb", closefd=True) as destination_handle:
                descriptor = -1
                shutil.copyfileobj(source_handle, destination_handle, length=1024 * 1024)
                destination_handle.flush()
                os.fsync(destination_handle.fileno())
        temporary.chmod(0o600)
        inspect_esp32s3_application_image(temporary)
        temporary.chmod(0o644)
        os.replace(temporary, destination)
        return destination
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def is_setup_complete(state: PersistedState) -> bool:
    if state.setup_version < SETUP_VERSION:
        return False
    if not state.paired_device_id:
        return False
    if not state.real_codex_path:
        return False
    if not state.helper_app_path or not Path(state.helper_app_path).exists():
        return False
    if not state.shim_dir:
        return False
    shim_path = Path(state.shim_dir) / "codex"
    if not shim_path.is_file() or not os.access(shim_path, os.X_OK):
        return False
    if not state.shell_integrated:
        return False
    if not state.service_installed:
        return False
    return True
