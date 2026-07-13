from __future__ import annotations

import os
import shutil
from pathlib import Path

from . import runtime
from .ble_transport import _native_helper_app_path
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
    executable = destination / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
    if executable.exists():
        return destination

    source = _native_helper_app_path()
    if source == destination:
        return destination

    if destination.exists():
        shutil.rmtree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)
    return destination


def ensure_firmware_artifact_installed(source: Path, destination: Path | None = None) -> Path:
    destination = runtime.default_firmware_path() if destination is None else Path(destination)
    source = Path(source)
    if source.is_symlink() or not source.is_file():
        return destination
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    temporary = destination.with_name(destination.name + ".tmp")
    temporary.unlink(missing_ok=True)
    shutil.copyfile(source, temporary, follow_symlinks=False)
    temporary.chmod(0o644)
    os.replace(temporary, destination)
    return destination


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
