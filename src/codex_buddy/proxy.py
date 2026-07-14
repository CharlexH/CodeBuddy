from __future__ import annotations

import asyncio
import json
import re
import shlex
from dataclasses import dataclass
from typing import Any, Awaitable, Callable, Dict, Optional

import websockets
from websockets.server import WebSocketServer, WebSocketServerProtocol

from .events import ApprovalRequest, AgentOutput, TokenUsage, TurnState

JsonDict = dict[str, Any]
EventCallback = Callable[[object], Awaitable[None]]
CloseCallback = Callable[[], Awaitable[None]]
_SHELL_BINARIES = {"sh", "bash", "zsh", "/bin/sh", "/bin/bash", "/bin/zsh"}
_READ_ONLY_SEGMENT_PREFIXES = (
    "test ",
    "[ ",
    "[[ ",
    "ls",
    "stat ",
    "wc ",
    "cat ",
    "sed -n ",
    "grep ",
    "rg ",
    "find ",
    "head ",
    "tail ",
)
_OUTPUT_SEGMENT_PREFIXES = ("echo ", "printf ")


def map_device_decision_to_codex_response(device_decision: str) -> dict[str, str]:
    if device_decision == "once":
        return {"decision": "accept"}
    if device_decision == "deny":
        return {"decision": "decline"}
    raise ValueError(f"Unsupported device decision: {device_decision}")


def is_read_only_verification_command(command: str) -> bool:
    script = _unwrap_shell_command(command)
    if not script:
        return False
    segments = [segment.strip() for segment in re.split(r"\s*(?:&&|;)\s*", script) if segment.strip()]
    if not segments:
        return False
    saw_verification = False
    for segment in segments:
        if segment in {":", "true"}:
            continue
        if any(segment.startswith(prefix) for prefix in _OUTPUT_SEGMENT_PREFIXES):
            continue
        if any(segment.startswith(prefix) for prefix in _READ_ONLY_SEGMENT_PREFIXES):
            saw_verification = True
            continue
        return False
    return saw_verification


def _unwrap_shell_command(command: str) -> str:
    text = command.strip()
    try:
        argv = shlex.split(text, posix=True)
    except ValueError:
        return text
    if len(argv) >= 3 and argv[0] in _SHELL_BINARIES and argv[1] in {"-c", "-lc"}:
        return argv[2].strip()
    return text


@dataclass
class PendingApproval:
    request_id: str
    thread_id: str
    turn_id: str
    command: str
    resolved: bool = False


class CodexEventSource:
    def __init__(
        self,
        *,
        upstream_url: str,
        listen_host: str,
        listen_port: int,
        on_event: EventCallback,
        on_close: Optional[CloseCallback] = None,
    ) -> None:
        self.upstream_url = upstream_url
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.on_event = on_event
        self.on_close = on_close
        self._server: Optional[WebSocketServer] = None
        self._upstream: Optional[websockets.WebSocketClientProtocol] = None
        self._pending: Dict[str, PendingApproval] = {}
        self._approved_mutating_turns: set[tuple[str, str]] = set()
        self._forward_lock = asyncio.Lock()

    async def start(self) -> None:
        self._server = await websockets.serve(self._handle_client, self.listen_host, self.listen_port)

    async def stop(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        self._server = None
        self._upstream = None
        self._pending.clear()
        self._approved_mutating_turns.clear()

    async def respond_to_device_approval(self, request_id: str, decision: str) -> None:
        pending = self._pending.get(str(request_id))
        if not pending or pending.resolved or self._upstream is None:
            return
        pending.resolved = True
        if decision == "once":
            self._record_approved_turn(pending)
        payload = {"id": self._coerce_id(request_id), "result": map_device_decision_to_codex_response(decision)}
        async with self._forward_lock:
            await self._upstream.send(json.dumps(payload))

    async def _handle_client(self, downstream: WebSocketServerProtocol) -> None:
        try:
            async with websockets.connect(self.upstream_url) as upstream:
                self._upstream = upstream
                await asyncio.gather(
                    self._forward_downstream(downstream, upstream),
                    self._forward_upstream(upstream, downstream),
                )
        finally:
            self._upstream = None
            if self.on_close is not None:
                await self.on_close()

    async def _forward_downstream(
        self,
        downstream: WebSocketServerProtocol,
        upstream: websockets.WebSocketClientProtocol,
    ) -> None:
        async for raw in downstream:
            message = json.loads(raw)
            if self._is_late_approval_response(message):
                continue
            async with self._forward_lock:
                await upstream.send(raw)

    async def _forward_upstream(
        self,
        upstream: websockets.WebSocketClientProtocol,
        downstream: WebSocketServerProtocol,
    ) -> None:
        async for raw in upstream:
            message = json.loads(raw)
            await self._emit_events(message)
            await downstream.send(raw)

    def _is_late_approval_response(self, message: JsonDict) -> bool:
        if "method" in message or "id" not in message or "result" not in message:
            return False
        request_id = str(message["id"])
        pending = self._pending.get(request_id)
        if pending is None:
            return False
        if pending.resolved:
            return True
        pending.resolved = True
        result = message.get("result", {})
        if isinstance(result, dict) and result.get("decision") == "accept":
            self._record_approved_turn(pending)
        return False

    async def _emit_events(self, message: JsonDict) -> None:
        method = message.get("method")
        params = message.get("params", {})
        if method == "turn/started":
            await self.on_event(
                TurnState(
                    thread_id=str(params.get("threadId", "")),
                    turn_id=str(params.get("turn", {}).get("id", "")),
                    active=True,
                )
            )
            return
        if method == "turn/completed":
            turn = params.get("turn", {})
            self._approved_mutating_turns.discard(
                (str(params.get("threadId", "")), str(turn.get("id", "")))
            )
            await self.on_event(
                TurnState(
                    thread_id=str(params.get("threadId", "")),
                    turn_id=str(turn.get("id", "")),
                    active=False,
                    status=str(turn.get("status", "completed")),
                )
            )
            return
        if method == "thread/tokenUsage/updated":
            usage = params.get("usage", {})
            await self.on_event(
                TokenUsage(
                    thread_id=str(params.get("threadId", "")),
                    total_tokens=int(usage.get("outputTokens", 0)),
                    tokens_today=int(usage.get("sessionOutputTokens", usage.get("outputTokens", 0))),
                )
            )
            return
        if method == "item/commandExecution/requestApproval":
            request_id = str(message["id"])
            approval = PendingApproval(
                request_id=request_id,
                thread_id=str(params.get("threadId", "")),
                turn_id=str(params.get("turnId", "")),
                command=str(params.get("command", params.get("reason", ""))),
            )
            self._pending[request_id] = approval
            if self._should_auto_accept(approval):
                approval.resolved = True
                async with self._forward_lock:
                    await self._upstream.send(
                        json.dumps(
                            {
                                "id": self._coerce_id(request_id),
                                "result": map_device_decision_to_codex_response("once"),
                            }
                        )
                    )
                return
            await self.on_event(
                ApprovalRequest(
                    thread_id=approval.thread_id,
                    turn_id=approval.turn_id,
                    request_id=request_id,
                    command=approval.command,
                    cwd=str(params.get("cwd", "")),
                    reason=str(params.get("reason", "")),
                )
            )
            return
        if method == "serverRequest/resolved":
            request_id = str(params.get("requestId", ""))
            pending = self._pending.get(request_id)
            if pending is not None:
                pending.resolved = True
            await self.on_event(ApprovalRequestResolved(request_id=request_id))
            return
        if method == "item/completed":
            item = params.get("item", {})
            item_type = item.get("type")
            thread_id = str(params.get("threadId", ""))
            if item_type == "agentMessage":
                await self.on_event(AgentOutput(thread_id=thread_id, text=str(item.get("text", ""))))
            elif item_type == "userMessage":
                texts = [
                    str(content.get("text", ""))
                    for content in item.get("content", [])
                    if content.get("type") == "text"
                ]
                if texts:
                    await self.on_event(AgentOutput(thread_id=thread_id, text=texts[0]))
            elif item_type == "commandExecution":
                await self.on_event(AgentOutput(thread_id=thread_id, text=str(item.get("command", ""))))

    @staticmethod
    def _coerce_id(request_id: str) -> Any:
        try:
            return int(request_id)
        except ValueError:
            return request_id

    def _record_approved_turn(self, pending: PendingApproval) -> None:
        if pending.thread_id and pending.turn_id and not is_read_only_verification_command(pending.command):
            self._approved_mutating_turns.add((pending.thread_id, pending.turn_id))

    def _should_auto_accept(self, pending: PendingApproval) -> bool:
        if self._upstream is None:
            return False
        if not is_read_only_verification_command(pending.command):
            return False
        return (pending.thread_id, pending.turn_id) in self._approved_mutating_turns


@dataclass(frozen=True)
class ApprovalRequestResolved:
    request_id: str
