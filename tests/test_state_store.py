from datetime import datetime, timezone

from codex_buddy.state_store import BridgeStateStore, PersistedState


def test_state_store_resets_tokens_today_after_local_midnight(tmp_path):
    path = tmp_path / "state.json"
    store = BridgeStateStore(path)

    store.save(
        PersistedState(
            paired_device_id="AA:BB:CC:DD",
            tokens_today=77,
            tokens_date="2026-04-19",
            tokens_total=1200,
            completion_seq=41,
            active_thread_id="thr_1",
        )
    )

    loaded = store.load(now=datetime(2026, 4, 20, 9, 0, tzinfo=timezone.utc))

    assert loaded.paired_device_id == "AA:BB:CC:DD"
    assert loaded.tokens_total == 1200
    assert loaded.tokens_today == 0
    assert loaded.tokens_date == "2026-04-20"
    assert loaded.completion_seq == 41


def test_state_store_round_trips_when_day_has_not_changed(tmp_path):
    path = tmp_path / "state.json"
    store = BridgeStateStore(path)

    original = PersistedState(
        paired_device_id="AA:BB:CC:DD",
        tokens_today=12,
        tokens_date="2026-04-20",
        tokens_total=34,
        completion_seq=42,
        active_thread_id="thr_2",
    )
    store.save(original)

    loaded = store.load(now=datetime(2026, 4, 20, 10, 30, tzinfo=timezone.utc))

    assert loaded == original


def test_state_store_preserves_setup_metadata_across_midnight_reset(tmp_path):
    path = tmp_path / "state.json"
    store = BridgeStateStore(path)

    store.save(
        PersistedState(
            paired_device_id="AA:BB:CC:DD",
            paired_device_name="Codex-1234",
            tokens_today=77,
            tokens_date="2026-04-19",
            tokens_total=1200,
            active_thread_id="thr_1",
            setup_version=1,
            real_codex_path="/usr/local/bin/codex",
            codex_launch_path="/usr/local/bin:/usr/bin:/bin",
            helper_app_path="/Users/tester/.code-buddy/helper/CodeBuddyBLEHelper.app",
            shim_dir="/Users/tester/.code-buddy/bin",
            shell_integrated=True,
            service_installed=True,
        )
    )

    loaded = store.load(now=datetime(2026, 4, 20, 9, 0, tzinfo=timezone.utc))

    assert loaded.tokens_today == 0
    assert loaded.tokens_total == 1200
    assert loaded.setup_version == 1
    assert loaded.real_codex_path == "/usr/local/bin/codex"
    assert loaded.codex_launch_path == "/usr/local/bin:/usr/bin:/bin"
    assert loaded.helper_app_path == "/Users/tester/.code-buddy/helper/CodeBuddyBLEHelper.app"
    assert loaded.shim_dir == "/Users/tester/.code-buddy/bin"
    assert loaded.shell_integrated is True
    assert loaded.service_installed is True
