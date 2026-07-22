from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Optional

from .reducer import BuddySnapshot
from .text_width import clip_text_by_width

_PROMPT_HINT_LIMIT = 160


def clip_text(text: str, limit: int) -> str:
    return clip_text_by_width(text, limit, ellipsis="...")


@dataclass(frozen=True)
class SessionPrompt:
    request_id: str
    tool: str
    hint: str


@dataclass(frozen=True)
class SessionRecord:
    session_id: str
    source: str
    originator: str
    cwd: str
    state: str
    last_activity_at: float
    latest_message: str
    entries: list[str]
    tokens_total: int
    tokens_session: int
    control_capability: str
    pending_prompt: Optional[SessionPrompt] = None
    completed_turn_id: Optional[str] = None
    heartbeat_tokens_total: Optional[int] = None

    def as_dict(self) -> dict[str, object]:
        data = asdict(self)
        prompt = data.get("pending_prompt")
        if prompt is not None:
            data["pending_prompt"] = {
                "request_id": prompt["request_id"],
                "tool": prompt["tool"],
                "hint": prompt["hint"],
            }
        return data


class SessionCatalog:
    def __init__(self, *, active_window_seconds: float = 300.0, completed_window_seconds: float = 120.0) -> None:
        self.active_window_seconds = active_window_seconds
        self.completed_window_seconds = completed_window_seconds
        self._sessions: dict[str, SessionRecord] = {}
        self._request_to_session: dict[str, str] = {}

    def upsert(self, session: SessionRecord) -> None:
        self._sessions[session.session_id] = session
        if session.pending_prompt is not None:
            self._request_to_session[session.pending_prompt.request_id] = session.session_id
        else:
            self._drop_request_mapping_for_session(session.session_id)

    def remove(self, session_id: str) -> None:
        self._sessions.pop(session_id, None)
        self._drop_request_mapping_for_session(session_id)

    def replace_readonly(self, sessions: list[SessionRecord]) -> None:
        readonly_ids = {session.session_id for session in sessions}
        for existing_id, existing in list(self._sessions.items()):
            if existing.control_capability == "readonly" and existing_id not in readonly_ids:
                self.remove(existing_id)
        for session in sessions:
            existing = self._sessions.get(session.session_id)
            if existing is not None and existing.control_capability == "managed":
                continue
            self.upsert(session)

    def resolve_prompt(self, request_id: str) -> None:
        session_id = self._request_to_session.pop(str(request_id), None)
        if session_id is None:
            return
        session = self._sessions.get(session_id)
        if session is None:
            return
        self._sessions[session_id] = SessionRecord(
            session_id=session.session_id,
            source=session.source,
            originator=session.originator,
            cwd=session.cwd,
            state="running" if session.state == "waiting" else session.state,
            last_activity_at=session.last_activity_at,
            latest_message=session.latest_message,
            entries=session.entries,
            tokens_total=session.tokens_total,
            tokens_session=session.tokens_session,
            control_capability=session.control_capability,
            pending_prompt=None,
            heartbeat_tokens_total=session.heartbeat_tokens_total,
        )

    def session_for_request(self, request_id: str) -> Optional[str]:
        return self._request_to_session.get(str(request_id))

    def sessions(self, *, now: float) -> list[SessionRecord]:
        visible = self._visible_sessions(now)
        return sorted(visible, key=lambda session: (self._priority(session), session.last_activity_at), reverse=True)

    def snapshot(self, *, now: float) -> BuddySnapshot:
        visible = self.sessions(now=now)
        waiting_sessions = [session for session in visible if session.state == "waiting"]
        managed_waiting = [
            session
            for session in waiting_sessions
            if session.control_capability == "managed" and session.pending_prompt is not None
        ]

        prompt: Optional[dict[str, str]] = None
        primary: Optional[SessionRecord] = None
        msg = "No Codex connected"
        entries: list[str] = []

        if len(managed_waiting) == 1:
            primary = managed_waiting[0]
            assert primary.pending_prompt is not None
            prompt = {
                "id": primary.pending_prompt.request_id,
                "tool": primary.pending_prompt.tool,
                "hint": clip_text(primary.pending_prompt.hint, _PROMPT_HINT_LIMIT),
            }
            msg = clip_text(primary.latest_message or f"approve: {primary.pending_prompt.hint}", 44)
            entries = primary.entries[:3]
        elif len(managed_waiting) > 1:
            primary = managed_waiting[0]
            msg = f"{len(managed_waiting)} approvals waiting; open on host"
            entries = primary.entries[:3]
        elif waiting_sessions:
            primary = waiting_sessions[0]
            msg = "approval pending on host"
            entries = primary.entries[:3]
        elif visible:
            primary = visible[0]
            msg = clip_text(primary.latest_message or "Codex is working", 44)
            entries = primary.entries[:3]

        return BuddySnapshot(
            total=len(visible),
            running=sum(1 for session in visible if session.state == "running"),
            waiting=len(waiting_sessions),
            msg=msg,
            entries=entries,
            tokens=sum(max(0, session.tokens_total) for session in visible),
            tokens_today=sum(max(0, session.tokens_session) for session in visible),
            prompt=prompt,
        )

    def _visible_sessions(self, now: float) -> list[SessionRecord]:
        visible: list[SessionRecord] = []
        for session in self._sessions.values():
            age = max(0.0, now - session.last_activity_at)
            if session.state in {"running", "waiting"}:
                visible.append(session)
                continue
            if session.state == "recent" and age <= self.active_window_seconds:
                visible.append(session)
                continue
            if session.state == "completed" and age <= self.completed_window_seconds:
                visible.append(session)
                continue
        return visible

    @staticmethod
    def _priority(session: SessionRecord) -> int:
        if session.state == "waiting":
            return 4
        if session.state == "running":
            return 3
        if session.state == "recent":
            return 2
        if session.state == "completed":
            return 1
        return 0

    def _drop_request_mapping_for_session(self, session_id: str) -> None:
        stale = [request_id for request_id, mapped_session in self._request_to_session.items() if mapped_session == session_id]
        for request_id in stale:
            self._request_to_session.pop(request_id, None)
