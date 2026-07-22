from __future__ import annotations

from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class TurnState:
    thread_id: str
    turn_id: str
    active: bool
    status: str = ""


@dataclass(frozen=True)
class AgentOutput:
    thread_id: str
    text: str


@dataclass(frozen=True)
class TokenUsage:
    thread_id: str
    total_tokens: int
    tokens_today: int
    heartbeat_total_tokens: Optional[int] = None


@dataclass(frozen=True)
class ApprovalRequest:
    thread_id: str
    turn_id: str
    request_id: str
    command: str
    cwd: str
    reason: str
    tool: str = "Bash"
    hint: Optional[str] = None
