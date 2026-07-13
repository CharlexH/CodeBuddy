from pathlib import Path

from codex_buddy import runtime


def test_runtime_root_uses_code_buddy_home():
    assert runtime.runtime_root() == Path.home() / ".code-buddy"


def test_legacy_runtime_root_kept_for_migration():
    assert runtime.legacy_runtime_root() == Path.home() / ".codex-buddy"


def test_runtime_paths_are_derived_from_runtime_root():
    root = Path.home() / ".code-buddy"

    assert runtime.state_path() == root / "state.json"
    assert runtime.logs_dir() == root / "logs"
    assert runtime.shim_dir() == root / "bin"
    assert runtime.shim_path() == root / "bin" / "codex"
    assert runtime.helper_app_path() == root / "helper" / "CodeBuddyBLEHelper.app"
    assert runtime.socket_path() == root / "agent.sock"
    assert runtime.ota_snapshots_dir() == root / "ota" / "snapshots"
    assert runtime.ota_sessions_dir() == root / "ota" / "private" / "sessions"


def test_zprofile_path_is_default_shell_integration_target():
    assert runtime.zprofile_path() == Path.home() / ".zprofile"
