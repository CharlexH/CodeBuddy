from __future__ import annotations

import json
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from .catalog import SessionRecord
from .text_width import clip_text_by_width

_ENTRY_LIMIT = 160
_LATEST_MESSAGE_LIMIT = 160


def parse_session_log(
    path: Path,
    *,
    now: Optional[float] = None,
    active_window_seconds: float = 300.0,
    completed_window_seconds: float = 120.0,
) -> Optional[SessionRecord]:
    now_ts = time.time() if now is None else float(now)

    try:
        handle = path.open("r", encoding="utf-8")
    except OSError:
        return None

    session_id: Optional[str] = None
    source = ""
    originator = ""
    cwd = ""
    last_activity_at: Optional[float] = None
    last_started_at: Optional[float] = None
    last_completed_at: Optional[float] = None
    last_completed_turn_id: Optional[str] = None
    latest_message = ""
    entries: list[str] = []
    last_entry_value: Optional[str] = None
    tokens_total = 0
    tokens_session = 0

    with handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue

            record_ts = _coerce_timestamp(record.get("timestamp"))
            record_type = record.get("type")
            payload = record.get("payload")
            if not isinstance(payload, dict):
                continue

            if record_type == "session_meta":
                payload_id = payload.get("id")
                if payload_id is not None:
                    session_id = str(payload_id)
                source = _normalize_source(payload.get("source"))
                originator = str(payload.get("originator") or "")
                cwd = str(payload.get("cwd") or "")
                last_activity_at = _max_timestamp(last_activity_at, record_ts, _coerce_timestamp(payload.get("timestamp")))
                continue

            if record_type != "event_msg" and record_type != "response_item":
                continue

            if record_type == "event_msg":
                event_type = payload.get("type")
                if event_type == "task_started":
                    started_at = _coerce_timestamp(payload.get("started_at"))
                    if started_at is None:
                        started_at = record_ts
                    last_started_at = _max_timestamp(last_started_at, started_at, record_ts)
                    last_activity_at = _max_timestamp(last_activity_at, started_at)
                    continue
                if event_type == "task_complete":
                    completed_at = _coerce_timestamp(payload.get("completed_at"))
                    if completed_at is None:
                        completed_at = record_ts
                    last_completed_at = _max_timestamp(last_completed_at, completed_at, record_ts)
                    turn_id = payload.get("turn_id")
                    if isinstance(turn_id, str) and turn_id:
                        last_completed_turn_id = turn_id
                    last_activity_at = _max_timestamp(last_activity_at, completed_at)
                    last_agent_message = _clean_message(payload.get("last_agent_message"))
                    if last_agent_message:
                        latest_message = clip_text_by_width(last_agent_message, _LATEST_MESSAGE_LIMIT, ellipsis="...")
                        entries, last_entry_value = _append_entry(entries, last_entry_value, last_agent_message)
                    continue
                if event_type == "agent_message":
                    message = _clean_message(payload.get("message"))
                    if message:
                        latest_message = clip_text_by_width(message, _LATEST_MESSAGE_LIMIT, ellipsis="...")
                        entries, last_entry_value = _append_entry(entries, last_entry_value, message)
                    last_activity_at = _max_timestamp(last_activity_at, record_ts)
                    continue
                if event_type == "token_count":
                    info = payload.get("info")
                    if isinstance(info, dict):
                        total_token_usage = _coerce_token_count(info.get("total_token_usage"))
                        if total_token_usage is not None:
                            tokens_total = total_token_usage
                        last_token_usage = _coerce_token_count(info.get("last_token_usage"))
                        if last_token_usage is not None:
                            tokens_session = last_token_usage
                    last_activity_at = _max_timestamp(last_activity_at, record_ts)
                    continue
                continue

            if payload.get("type") != "message":
                continue
            role = str(payload.get("role") or "")
            if role not in {"assistant", "user"}:
                continue
            message = _extract_response_message(payload)
            if not message:
                continue
            latest_message = clip_text_by_width(message, _LATEST_MESSAGE_LIMIT, ellipsis="...")
            entries, last_entry_value = _append_entry(entries, last_entry_value, message)
            last_activity_at = _max_timestamp(last_activity_at, record_ts)

    if not session_id:
        return None

    if last_activity_at is None:
        try:
            last_activity_at = float(path.stat().st_mtime)
        except OSError:
            return None

    age = max(0.0, now_ts - last_activity_at)
    in_flight = last_started_at is not None and (
        last_completed_at is None or last_started_at > last_completed_at
    )
    if in_flight:
        if age > active_window_seconds:
            return None
        state = "running"
    elif age <= completed_window_seconds:
        state = "completed"
    elif age <= active_window_seconds:
        state = "recent"
    else:
        return None

    if not latest_message and entries:
        latest_message = entries[-1]

    return SessionRecord(
        session_id=session_id,
        source=source,
        originator=originator,
        cwd=cwd,
        state=state,
        last_activity_at=last_activity_at,
        latest_message=latest_message,
        entries=entries[-3:],
        tokens_total=tokens_total,
        tokens_session=tokens_session,
        control_capability="readonly",
        pending_prompt=None,
        completed_turn_id=last_completed_turn_id,
    )


class SessionLogWatcher:
    def __init__(
        self,
        root: Path,
        *,
        max_files: int = 200,
        active_window_seconds: float = 300.0,
        completed_window_seconds: float = 120.0,
    ) -> None:
        self.root = root
        self.max_files = max(0, int(max_files))
        self.active_window_seconds = active_window_seconds
        self.completed_window_seconds = completed_window_seconds

    def poll(self, now: Optional[float] = None) -> list[SessionRecord]:
        now_ts = time.time() if now is None else float(now)
        records_by_id: dict[str, SessionRecord] = {}

        for path in self._candidate_paths(now=now_ts):
            session = parse_session_log(
                path,
                now=now_ts,
                active_window_seconds=self.active_window_seconds,
                completed_window_seconds=self.completed_window_seconds,
            )
            if session is None:
                continue
            existing = records_by_id.get(session.session_id)
            if existing is None or session.last_activity_at >= existing.last_activity_at:
                records_by_id[session.session_id] = session

        return sorted(records_by_id.values(), key=lambda session: session.last_activity_at, reverse=True)

    def _candidate_paths(self, *, now: Optional[float] = None) -> list[Path]:
        if self.max_files <= 0:
            return []
        if self.root.is_file():
            return [self.root]
        if not self.root.exists():
            return []

        now_ts = time.time() if now is None else float(now)
        cutoff = now_ts - max(self.active_window_seconds, self.completed_window_seconds)
        candidates: list[tuple[float, Path]] = []
        for path in self.root.rglob("*.jsonl"):
            try:
                stat = path.stat()
            except OSError:
                continue
            mtime = float(stat.st_mtime)
            if mtime < cutoff:
                continue
            candidates.append((mtime, path))

        candidates.sort(key=lambda item: (item[0], str(item[1])), reverse=True)
        return [path for _, path in candidates[: self.max_files]]


def _extract_response_message(payload: dict[str, Any]) -> str:
    content = payload.get("content")
    if not isinstance(content, list):
        return ""
    parts: list[str] = []
    for item in content:
        if not isinstance(item, dict):
            continue
        item_type = item.get("type")
        if item_type not in {"input_text", "output_text"}:
            continue
        text = _clean_message(item.get("text"))
        if text:
            parts.append(text)
    return " ".join(parts).strip()


def _append_entry(entries: list[str], last_entry_value: Optional[str], message: str) -> tuple[list[str], Optional[str]]:
    entry_value = clip_text_by_width(message, _ENTRY_LIMIT, ellipsis="...")
    if not entry_value or entry_value == last_entry_value:
        return entries, last_entry_value
    entries.append(entry_value)
    return entries, entry_value


def _normalize_source(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        if "subagent" in value:
            return "subagent"
        keys = sorted(str(key) for key in value.keys())
        return ",".join(keys)
    return ""


def _clean_message(value: Any) -> str:
    if not isinstance(value, str):
        return ""
    message = clip_text_by_width(value, _LATEST_MESSAGE_LIMIT, ellipsis="...")
    if _is_noise_message(message):
        return ""
    return message


def _is_noise_message(message: str) -> bool:
    stripped = message.strip()
    return stripped.startswith("<environment_context>") or stripped.startswith("<turn_aborted>")


def _coerce_token_count(value: Any) -> Optional[int]:
    if not isinstance(value, dict):
        return None
    total = value.get("total_tokens")
    if isinstance(total, bool):
        return None
    if isinstance(total, (int, float)):
        return int(total)
    return None


def _coerce_timestamp(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if not isinstance(value, str):
        return None
    text = value.strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        pass
    if text.endswith("Z"):
        text = text[:-1] + "+00:00"
    try:
        return datetime.fromisoformat(text).astimezone(timezone.utc).timestamp()
    except ValueError:
        return None


def _max_timestamp(current: Optional[float], *candidates: Optional[float]) -> Optional[float]:
    values = [value for value in candidates if value is not None]
    if current is not None:
        values.append(current)
    if not values:
        return None
    return max(values)
