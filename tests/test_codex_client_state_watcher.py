import json
from pathlib import Path

from codex_buddy.codex_client_state_watcher import CodexClientStateWatcher


def _write_state(path: Path, local: object, *, remote: object = None) -> None:
    unread_by_host = {"local": local}
    if remote is not None:
        unread_by_host["ssh:example"] = remote
    path.write_text(
        json.dumps(
            {
                "electron-persisted-atom-state": {
                    "unread-thread-ids-by-host-v1": unread_by_host
                }
            }
        ),
        encoding="utf-8",
    )


def test_counts_unique_local_unread_thread_ids_only(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["thread-a", "thread-b"], remote=["remote-a"])

    assert CodexClientStateWatcher(path).poll({"thread-b", "remote-a"}) == 1


def test_empty_visible_thread_set_clears_stale_historical_unread_ids(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["archived-a", "archived-b"])

    assert CodexClientStateWatcher(path).poll(set()) == 0


def test_unread_is_unknown_until_app_server_visible_threads_are_available(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["thread-a"])

    assert CodexClientStateWatcher(path).poll(None) is None


def test_missing_or_malformed_state_is_unknown_before_first_valid_read(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    watcher = CodexClientStateWatcher(path)

    assert watcher.poll({"thread-a"}) is None
    path.write_text("{", encoding="utf-8")
    assert watcher.poll({"thread-a"}) is None


def test_transient_failure_retains_last_trusted_count(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["thread-a"])
    watcher = CodexClientStateWatcher(path)

    assert watcher.poll({"thread-a"}) == 1
    path.write_text("{", encoding="utf-8")
    assert watcher.poll({"thread-a"}) == 1


def test_invalid_ids_do_not_replace_last_trusted_count(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["thread-a"])
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll({"thread-a"}) == 1

    _write_state(path, ["thread-a", "thread-a"])
    assert watcher.poll({"thread-a"}) == 1

    _write_state(path, ["thread-a", 7])
    assert watcher.poll({"thread-a"}) == 1


def test_valid_state_can_update_after_a_transient_failure(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    _write_state(path, ["thread-a"])
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll({"thread-a"}) == 1

    path.unlink()
    assert watcher.poll({"thread-a"}) == 1

    _write_state(path, ["thread-a", "thread-b", "thread-c"])
    assert watcher.poll({"thread-a", "thread-b", "thread-c"}) == 3
