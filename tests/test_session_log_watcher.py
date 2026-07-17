import json
import os
from datetime import datetime, timezone
from pathlib import Path

import pytest

from codex_buddy.session_log_watcher import SessionLogWatcher, parse_session_log


def _iso(ts: float) -> str:
    return datetime.fromtimestamp(ts, tz=timezone.utc).isoformat().replace("+00:00", "Z")


def _write_log(path: Path, records: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")


def _session_meta(*, session_id: str, cwd: str = "/tmp/project", source: str = "vscode", originator: str = "Codex Desktop"):
    return {
        "timestamp": _iso(1_000.0),
        "type": "session_meta",
        "payload": {
            "id": session_id,
            "timestamp": _iso(1_000.0),
            "cwd": cwd,
            "source": source,
            "originator": originator,
        },
    }


def _event(ts: float, event_type: str, **payload):
    data = {"type": event_type}
    data.update(payload)
    return {
        "timestamp": _iso(ts),
        "type": "event_msg",
        "payload": data,
    }


def _message(ts: float, role: str, text: str):
    content_type = "input_text" if role == "user" else "output_text"
    return {
        "timestamp": _iso(ts),
        "type": "response_item",
        "payload": {
            "type": "message",
            "role": role,
            "content": [{"type": content_type, "text": text}],
        },
    }


def test_parse_session_log_returns_running_record_with_latest_entries_and_tokens(tmp_path: Path):
    log_path = tmp_path / "sessions" / "running.jsonl"
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-running"),
            _event(1_000.0, "task_started", turn_id="turn-1", started_at=1_000.0),
            _message(
                1_000.5,
                "user",
                "<environment_context>\n  <cwd>/tmp/project</cwd>\n</environment_context>",
            ),
            _message(1_001.0, "user", "Please   inspect\n the repo"),
            _event(1_002.0, "agent_message", message="Reading   files now"),
            _message(1_002.1, "assistant", "Reading   files now"),
            _message(1_003.0, "user", "Focus on  proxy.py"),
            _event(1_004.0, "agent_message", message="Looking at proxy.py and related tests now."),
            _event(
                1_005.0,
                "token_count",
                info={
                    "total_token_usage": {"total_tokens": 321},
                    "last_token_usage": {"total_tokens": 55},
                },
            ),
        ],
    )

    session = parse_session_log(log_path, now=1_050.0)

    assert session is not None
    assert session.session_id == "session-running"
    assert session.source == "vscode"
    assert session.originator == "Codex Desktop"
    assert session.cwd == "/tmp/project"
    assert session.state == "running"
    assert session.last_activity_at == pytest.approx(1_005.0)
    assert session.latest_message == "Looking at proxy.py and related tests now."
    assert session.entries == [
        "Reading files now",
        "Focus on proxy.py",
        "Looking at proxy.py and related tests now.",
    ]
    assert session.tokens_total == 321
    assert session.tokens_session == 55
    assert session.control_capability == "readonly"
    assert session.pending_prompt is None


def test_parse_session_log_transitions_from_completed_to_recent_then_drops(tmp_path: Path):
    log_path = tmp_path / "archived" / "completed.jsonl"
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-complete", cwd="/tmp/complete"),
            _event(2_000.0, "task_started", turn_id="turn-2", started_at=2_000.0),
            _message(2_001.0, "user", "Do the thing"),
            _event(2_005.0, "agent_message", message="Done."),
            _event(
                2_010.0,
                "token_count",
                info={
                    "total_token_usage": {"total_tokens": 900},
                    "last_token_usage": {"total_tokens": 120},
                },
            ),
            _event(
                2_020.0,
                "task_complete",
                turn_id="turn-2",
                completed_at=2_020.0,
                last_agent_message="Done.",
            ),
        ],
    )

    completed = parse_session_log(
        log_path,
        now=2_060.0,
        active_window_seconds=300.0,
        completed_window_seconds=120.0,
    )
    assert completed is not None
    assert completed.state == "completed"
    assert getattr(completed, "completed_turn_id", None) == "turn-2"
    assert completed.latest_message == "Done."
    assert completed.entries == ["Do the thing", "Done."]

    recent = parse_session_log(
        log_path,
        now=2_200.0,
        active_window_seconds=300.0,
        completed_window_seconds=120.0,
    )
    assert recent is not None
    assert recent.state == "recent"

    assert (
        parse_session_log(
            log_path,
            now=2_400.0,
            active_window_seconds=300.0,
            completed_window_seconds=120.0,
        )
        is None
    )


def test_parse_session_log_treats_same_second_restart_as_running(tmp_path: Path):
    log_path = tmp_path / "sessions" / "same-second-restart.jsonl"
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-restart"),
            _event(3_000.000, "task_started", turn_id="turn-1", started_at=3_000.0),
            _event(
                3_010.400,
                "task_complete",
                turn_id="turn-1",
                completed_at=3_010.0,
                last_agent_message="First turn done.",
            ),
            _event(3_010.900, "task_started", turn_id="turn-2", started_at=3_010.0),
            _event(3_011.100, "agent_message", message="Second turn is running."),
        ],
    )

    session = parse_session_log(log_path, now=3_040.0)

    assert session is not None
    assert session.state == "running"
    assert session.latest_message == "Second turn is running."


def test_session_log_watcher_poll_rescans_recursively_and_honors_max_files(tmp_path: Path):
    root = tmp_path / "logs"
    first_path = root / "2026" / "04" / "20" / "first.jsonl"
    ignored_path = root / "old" / "stale.jsonl"

    _write_log(
        first_path,
        [
            _session_meta(session_id="session-1", cwd="/tmp/one"),
            _event(3_000.0, "task_started", turn_id="turn-a", started_at=3_000.0),
            _event(3_005.0, "agent_message", message="Working in session one"),
        ],
    )
    _write_log(
        ignored_path,
        [
            _session_meta(session_id="session-stale", cwd="/tmp/stale"),
            _event(1_000.0, "task_started", turn_id="turn-old", started_at=1_000.0),
            _event(1_010.0, "task_complete", turn_id="turn-old", completed_at=1_010.0),
        ],
    )
    os.utime(first_path, (3_030.0, 3_030.0))
    os.utime(ignored_path, (10.0, 10.0))

    watcher = SessionLogWatcher(root, max_files=2, active_window_seconds=300.0, completed_window_seconds=120.0)

    first_poll = watcher.poll(now=3_050.0)

    assert [session.session_id for session in first_poll] == ["session-1"]

    second_path = root / "2026" / "04" / "20" / "second.jsonl"
    third_path = root / "2026" / "04" / "20" / "third.jsonl"
    _write_log(
        second_path,
        [
            _session_meta(session_id="session-2", cwd="/tmp/two"),
            _event(3_100.0, "task_started", turn_id="turn-b", started_at=3_100.0),
            _event(3_110.0, "agent_message", message="Working in session two"),
        ],
    )
    _write_log(
        third_path,
        [
            _session_meta(session_id="session-3", cwd="/tmp/three"),
            _event(3_120.0, "task_started", turn_id="turn-c", started_at=3_120.0),
            _event(3_130.0, "agent_message", message="Working in session three"),
        ],
    )
    os.utime(second_path, (3_140.0, 3_140.0))
    os.utime(third_path, (3_145.0, 3_145.0))

    second_poll = watcher.poll(now=3_150.0)

    assert [session.session_id for session in second_poll] == ["session-3", "session-2"]


def test_session_log_watcher_skips_files_older_than_activity_window(tmp_path: Path):
    root = tmp_path / "logs"
    fresh_path = root / "2026" / "04" / "24" / "fresh.jsonl"
    stale_path = root / "2026" / "04" / "23" / "stale.jsonl"

    _write_log(
        fresh_path,
        [
            _session_meta(session_id="session-fresh", cwd="/tmp/fresh"),
            _event(5_000.0, "task_started", turn_id="turn-fresh", started_at=5_000.0),
            _event(5_010.0, "agent_message", message="Still active"),
        ],
    )
    _write_log(
        stale_path,
        [
            _session_meta(session_id="session-stale", cwd="/tmp/stale"),
            _event(4_000.0, "task_started", turn_id="turn-stale", started_at=4_000.0),
            _event(4_010.0, "agent_message", message="Too old to matter"),
        ],
    )
    os.utime(fresh_path, (5_020.0, 5_020.0))
    os.utime(stale_path, (4_100.0, 4_100.0))

    watcher = SessionLogWatcher(root, max_files=200, active_window_seconds=300.0, completed_window_seconds=120.0)

    assert watcher._candidate_paths(now=5_100.0) == [fresh_path]
    assert [session.session_id for session in watcher.poll(now=5_100.0)] == ["session-fresh"]


def test_parse_session_log_normalizes_structured_source_values(tmp_path: Path):
    log_path = tmp_path / "sessions" / "subagent.jsonl"
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-subagent", source={"subagent": {"depth": 1}}),
            _event(4_000.0, "task_started", turn_id="turn-1", started_at=4_000.0),
            _event(4_005.0, "agent_message", message="Running delegated task"),
        ],
    )

    session = parse_session_log(log_path, now=4_030.0)

    assert session is not None
    assert session.source == "subagent"


def test_parse_session_log_clips_cjk_messages_by_display_width(tmp_path: Path):
    log_path = tmp_path / "sessions" / "cjk.jsonl"
    message = "你" * 25
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-cjk"),
            _event(5_000.0, "task_started", turn_id="turn-1", started_at=5_000.0),
            _event(5_005.0, "agent_message", message=message),
        ],
    )

    session = parse_session_log(log_path, now=5_020.0)

    assert session is not None
    assert session.latest_message == message
    assert session.entries == [message]


def test_parse_session_log_clips_cjk_entries_by_display_width(tmp_path: Path):
    log_path = tmp_path / "sessions" / "cjk.jsonl"
    _write_log(
        log_path,
        [
            _session_meta(session_id="session-cjk"),
            _event(5_000.0, "task_started", turn_id="turn-1", started_at=5_000.0),
            _event(5_005.0, "agent_message", message="你" * 25),
        ],
    )

    session = parse_session_log(log_path, now=5_020.0)

    assert session is not None
    assert session.entries == ["你" * 25]
