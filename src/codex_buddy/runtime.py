from __future__ import annotations

from pathlib import Path


def legacy_runtime_root() -> Path:
    return Path.home() / ".codex-buddy"


def runtime_root() -> Path:
    return Path.home() / ".code-buddy"


def state_path() -> Path:
    return runtime_root() / "state.json"


def logs_dir() -> Path:
    return runtime_root() / "logs"


def shim_dir() -> Path:
    return runtime_root() / "bin"


def shim_path() -> Path:
    return shim_dir() / "codex"


def helper_dir() -> Path:
    return runtime_root() / "helper"


def helper_app_path() -> Path:
    return helper_dir() / "CodeBuddyBLEHelper.app"


def socket_path() -> Path:
    return runtime_root() / "agent.sock"


def ota_dir() -> Path:
    return runtime_root() / "ota"


def ota_private_dir() -> Path:
    return ota_dir() / "private"


def ota_public_dir() -> Path:
    return ota_dir() / "public"


def ota_releases_dir() -> Path:
    return ota_dir() / "releases"


def ota_sessions_dir() -> Path:
    return ota_private_dir() / "sessions"


def firmware_dir() -> Path:
    return runtime_root() / "firmware"


def default_firmware_path() -> Path:
    return firmware_dir() / "code-buddy-sticks3-app.bin"


def zprofile_path() -> Path:
    return Path.home() / ".zprofile"
