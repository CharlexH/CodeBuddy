from __future__ import annotations

import json
from collections import deque
from dataclasses import dataclass
from typing import Deque, Dict, Optional

from .events import ApprovalRequest, AgentOutput, TokenUsage, TurnState
from .text_width import clip_text_by_width
from .usage_limits import UsageDisplay

_SUMMARY_LIMIT = 44
_ENTRY_LIMIT = 160
_PROMPT_LIMIT = 160
_BLE_PAYLOAD_MAX_BYTES = 900


def _clip(text: str, limit: int) -> str:
    return clip_text_by_width(text, limit, ellipsis="…")


def _ble_json_size(payload: dict) -> int:
    return len(json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")) + 1


@dataclass(frozen=True)
class BuddySnapshot:
    total: int
    running: int
    waiting: int
    msg: str
    entries: list[str]
    tokens: int
    tokens_today: int
    prompt: Optional[dict[str, str]]
    usage: Optional[UsageDisplay] = None
    usage_is_known: bool = False

    def as_ble_payload(self) -> dict:
        payload = {
            "total": self.total,
            "running": self.running,
            "waiting": self.waiting,
            "msg": self.msg,
            "entries": self.entries,
            "tokens": self.tokens,
            "tokens_today": self.tokens_today,
        }
        if self.prompt is not None:
            payload["prompt"] = dict(self.prompt)
        if self.usage is not None:
            payload["usage"] = {
                "five_hour_remaining": self.usage.five_hour_remaining,
                "seven_day_remaining": self.usage.seven_day_remaining,
            }
        elif self.usage_is_known:
            payload["usage"] = None

        if _ble_json_size(payload) <= _BLE_PAYLOAD_MAX_BYTES:
            return payload

        # Oldest transcript lines are lowest-priority on-device detail. Drop
        # them first so running/waiting counts still fit inside the StickS3's
        # line buffer even with verbose multilingual host output.
        entries = list(payload["entries"])
        while entries and _ble_json_size({**payload, "entries": entries}) > _BLE_PAYLOAD_MAX_BYTES:
            entries.pop()
        payload["entries"] = entries

        if _ble_json_size(payload) <= _BLE_PAYLOAD_MAX_BYTES:
            return payload

        prompt = payload.get("prompt")
        if prompt is not None:
            hint = str(prompt.get("hint", ""))
            for limit in (96, 72, 48, 32):
                prompt["hint"] = clip_text_by_width(hint, limit, ellipsis="...")
                if _ble_json_size(payload) <= _BLE_PAYLOAD_MAX_BYTES:
                    return payload
            payload.pop("prompt", None)

        if _ble_json_size(payload) <= _BLE_PAYLOAD_MAX_BYTES:
            return payload

        msg = str(payload.get("msg", ""))
        for limit in (36, 28, 20, 12):
            payload["msg"] = clip_text_by_width(msg, limit, ellipsis="...")
            if _ble_json_size(payload) <= _BLE_PAYLOAD_MAX_BYTES:
                return payload

        return payload


class BuddyStateReducer:
    def __init__(self, *, tokens: int = 0, tokens_today: int = 0) -> None:
        self._active_turns: Dict[str, str] = {}
        self._entries: Deque[str] = deque(maxlen=3)
        self._msg = "No Codex connected"
        self._tokens = tokens
        self._tokens_today = tokens_today
        self._pending_approval: Optional[ApprovalRequest] = None

    def apply(self, event: object) -> None:
        if isinstance(event, TurnState):
            if event.active:
                self._active_turns[event.thread_id] = event.turn_id
                if not self._pending_approval:
                    self._msg = "Codex is working"
            else:
                self._active_turns.pop(event.thread_id, None)
                if not self._active_turns and not self._pending_approval:
                    self._msg = "No active Codex turn"
            return

        if isinstance(event, AgentOutput):
            if event.text.strip():
                entry = _clip(event.text, _ENTRY_LIMIT)
                self._entries.appendleft(entry)
                self._msg = _clip(entry, _SUMMARY_LIMIT)
            return

        if isinstance(event, TokenUsage):
            self._tokens = max(0, event.total_tokens)
            self._tokens_today = max(0, event.tokens_today)
            return

        if isinstance(event, ApprovalRequest):
            self._pending_approval = event
            command = event.hint or event.command or event.reason
            self._msg = _clip("approve: " + command, _SUMMARY_LIMIT)
            return

        raise TypeError(f"Unsupported event: {type(event)!r}")

    def resolve_approval(self, request_id: str) -> None:
        if self._pending_approval and self._pending_approval.request_id == str(request_id):
            self._pending_approval = None
            if self._active_turns:
                self._msg = "Codex is working"
            elif self._entries:
                self._msg = _clip(self._entries[0], 44)
            else:
                self._msg = "No active Codex turn"

    def snapshot(self) -> BuddySnapshot:
        running = 0 if self._pending_approval else len(self._active_turns)
        waiting = 1 if self._pending_approval else 0
        total = len(self._active_turns) if self._active_turns else waiting
        prompt = None
        if self._pending_approval is not None:
            prompt = {
                "id": self._pending_approval.request_id,
                "tool": self._pending_approval.tool,
                "hint": clip_text_by_width(
                    self._pending_approval.hint
                    or self._pending_approval.command
                    or self._pending_approval.reason,
                    _PROMPT_LIMIT,
                    ellipsis="…",
                ),
            }
        return BuddySnapshot(
            total=total,
            running=running,
            waiting=waiting,
            msg=self._msg,
            entries=list(self._entries),
            tokens=self._tokens,
            tokens_today=self._tokens_today,
            prompt=prompt,
        )
