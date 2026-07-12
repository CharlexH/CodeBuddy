from __future__ import annotations

import asyncio
import contextlib
import json
import subprocess
import sys
import time
from collections import deque
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Awaitable, Callable, Deque, Optional

from .ble_transport import BleBuddyTransport
from .bridge import ManagedSessionBridge
from .catalog import SessionCatalog, SessionPrompt, SessionRecord
from .account_usage_monitor import AccountUsageMonitor
from .events import ApprovalRequest, AgentOutput, TokenUsage, TurnState
from .proxy import ApprovalRequestResolved
from .runtime import logs_dir as runtime_logs_dir
from .runtime import socket_path as runtime_socket_path
from .runtime import state_path as runtime_state_path
from .state_store import BridgeStateStore, PersistedState
from .text_width import clip_text_by_width
from .usage_limits import UsageDisplay

_SUMMARY_LIMIT = 44
_ENTRY_LIMIT = 160
_PROMPT_HINT_LIMIT = 160

try:
    from .session_log_watcher import SessionLogWatcher
except ImportError:  # pragma: no cover - populated during the same rollout
    SessionLogWatcher = None  # type: ignore[assignment]


def default_socket_path(state_path: Path) -> Path:
    if Path(state_path) == runtime_state_path():
        return runtime_socket_path()
    return state_path.parent / "agent.sock"


def default_log_dir(state_path: Path) -> Path:
    if Path(state_path) == runtime_state_path():
        return runtime_logs_dir()
    return state_path.parent / "logs"


class AgentClientError(RuntimeError):
    pass


class AgentClient:
    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path

    async def request(self, payload: dict[str, object]) -> dict[str, object]:
        try:
            reader, writer = await asyncio.open_unix_connection(str(self.socket_path))
        except OSError as exc:
            raise AgentClientError(str(exc)) from exc

        try:
            writer.write((json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8"))
            await writer.drain()
            raw = await reader.readline()
        finally:
            writer.close()
            with contextlib.suppress(Exception):
                await writer.wait_closed()

        if not raw:
            raise AgentClientError("Agent closed the connection without a response")
        response = json.loads(raw.decode("utf-8"))
        if not response.get("ok", False):
            raise AgentClientError(str(response.get("error", "Unknown agent error")))
        return response


@dataclass
class ManagedSessionRuntime:
    control_id: str
    workdir: Path
    session_id: Optional[str] = None
    state: str = "recent"
    latest_message: str = ""
    last_activity_at: float = 0.0
    tokens_total: int = 0
    tokens_session: int = 0
    pending_prompt: Optional[SessionPrompt] = None
    entries: Deque[str] = field(default_factory=lambda: deque(maxlen=3))

    def apply(self, event: object, *, now: float) -> None:
        thread_id = getattr(event, "thread_id", None)
        if thread_id:
            self.session_id = str(thread_id)
        self.last_activity_at = now

        if isinstance(event, TurnState):
            self.state = "running" if event.active else "completed"
            if event.active and not self.latest_message:
                self.latest_message = "Codex is working"
            return

        if isinstance(event, AgentOutput):
            if event.text.strip():
                entry = clip_text_by_width(event.text, _ENTRY_LIMIT, ellipsis="...")
                self.entries.appendleft(entry)
                self.latest_message = clip_text_by_width(entry, _SUMMARY_LIMIT, ellipsis="...")
                if self.state not in {"running", "waiting"}:
                    self.state = "recent"
            return

        if isinstance(event, TokenUsage):
            self.tokens_total = max(0, event.total_tokens)
            self.tokens_session = max(0, event.tokens_today)
            return

        if isinstance(event, ApprovalRequest):
            hint = clip_text_by_width(event.hint or event.command or event.reason, _PROMPT_HINT_LIMIT, ellipsis="...")
            self.pending_prompt = SessionPrompt(request_id=event.request_id, tool=event.tool, hint=hint)
            self.state = "waiting"
            self.latest_message = clip_text_by_width("approve: " + hint, _SUMMARY_LIMIT, ellipsis="...")
            self.entries.appendleft(self.latest_message)
            return

        if isinstance(event, ApprovalRequestResolved):
            if self.pending_prompt is None or self.pending_prompt.request_id != str(event.request_id):
                return
            self.pending_prompt = None
            self.state = "running"
            return

        raise TypeError("Unsupported managed event: {!r}".format(type(event)))

    def close(self, *, now: float) -> None:
        self.last_activity_at = now
        if self.state not in {"waiting", "completed"}:
            self.state = "completed"
        self.pending_prompt = None

    def to_record(self) -> Optional[SessionRecord]:
        if not self.session_id:
            return None
        return SessionRecord(
            session_id=self.session_id,
            source="managed",
            originator="code-buddy",
            cwd=str(self.workdir),
            state=self.state,
            last_activity_at=self.last_activity_at,
            latest_message=self.latest_message,
            entries=list(self.entries),
            tokens_total=self.tokens_total,
            tokens_session=self.tokens_session,
            control_capability="managed",
            pending_prompt=self.pending_prompt,
        )


class BuddyAgent:
    def __init__(
        self,
        state_path: Path,
        *,
        socket_path: Optional[Path] = None,
        clock: Optional[Callable[[], float]] = None,
        readonly_poll_interval: float = 2.0,
        keepalive_interval: float = 10.0,
        reconnect_interval: float = 5.0,
        watcher: Optional[Any] = None,
        ble_factory: Optional[Callable[..., BleBuddyTransport]] = None,
        managed_session_factory: Optional[Callable[..., ManagedSessionBridge]] = None,
        account_usage_monitor_factory: Optional[Callable[..., AccountUsageMonitor]] = None,
    ) -> None:
        self.state_path = state_path
        self.socket_path = socket_path or default_socket_path(state_path)
        self.clock = clock or time.time
        self.readonly_poll_interval = readonly_poll_interval
        self.keepalive_interval = keepalive_interval
        self.reconnect_interval = reconnect_interval
        self.store = BridgeStateStore(state_path)
        self.catalog = SessionCatalog()
        self._watcher = watcher or (
            SessionLogWatcher(Path.home() / ".codex" / "sessions") if SessionLogWatcher is not None else None
        )
        self._ble_factory = ble_factory or BleBuddyTransport
        self._managed_session_factory = managed_session_factory or ManagedSessionBridge
        self._account_usage_monitor_factory = account_usage_monitor_factory or AccountUsageMonitor
        self._account_usage_monitor: Optional[AccountUsageMonitor] = None
        self._usage: Optional[UsageDisplay] = None
        # A newly started agent must clear a meter rendered by an older
        # process even if its first account-rate-limit read cannot complete.
        self._usage_is_known = True
        self._managed_sessions: dict[str, ManagedSessionBridge] = {}
        self._managed_runtime: dict[str, ManagedSessionRuntime] = {}
        self._request_to_control: dict[str, str] = {}
        self._tasks: list[asyncio.Task[None]] = []
        self._server: Optional[asyncio.AbstractServer] = None
        self._stopped = asyncio.Event()
        self._ble: Optional[BleBuddyTransport] = None
        self._ble_connected = False
        self._last_payload: Optional[dict[str, object]] = None
        self._launch_sequence = 0
        self._ota_session_lock = asyncio.Lock()
        self._ota_session_active = False

    @property
    def ota_session_active(self) -> bool:
        return self._ota_session_active

    @contextlib.asynccontextmanager
    async def ota_session(self):
        async with self._ota_session_lock:
            self._ota_session_active = True
            try:
                yield
            finally:
                self._ota_session_active = False

    async def run(self) -> None:
        self.socket_path.parent.mkdir(parents=True, exist_ok=True)
        if self.socket_path.exists():
            self.socket_path.unlink()

        self._server = await asyncio.start_unix_server(self._handle_client, path=str(self.socket_path))
        await self._start_account_usage_monitor()
        self._tasks = [
            asyncio.create_task(self._readonly_loop()),
            asyncio.create_task(self._ble_loop()),
            asyncio.create_task(self._keepalive_loop()),
        ]
        await self._publish_state(force=True)
        try:
            await self._stopped.wait()
        finally:
            await self.shutdown()

    async def shutdown(self) -> None:
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
            self._server = None
        for task in self._tasks:
            task.cancel()
        for task in self._tasks:
            with contextlib.suppress(asyncio.CancelledError):
                await task
        self._tasks = []
        for bridge in list(self._managed_sessions.values()):
            await bridge.stop()
        self._managed_sessions.clear()
        self._managed_runtime.clear()
        self._request_to_control.clear()
        monitor = self._account_usage_monitor
        self._account_usage_monitor = None
        if monitor is not None:
            with contextlib.suppress(Exception):
                await monitor.stop()
        if self._ble is not None:
            with contextlib.suppress(Exception):
                await self._ble.disconnect()
        self._ble = None
        self._ble_connected = False
        if self.socket_path.exists():
            self.socket_path.unlink()
        snapshot = self._snapshot()
        self._persist(snapshot, agent_running=False)

    async def launch(self, workdir: Path) -> dict[str, object]:
        self._launch_sequence += 1
        control_id = "managed-{}-{}".format(int(self.clock() * 1000), self._launch_sequence)
        runtime = ManagedSessionRuntime(control_id=control_id, workdir=workdir)
        current = self.store.load()
        bridge = self._managed_session_factory(
            workdir=workdir,
            codex_path=current.real_codex_path or "codex",
            codex_launch_path=current.codex_launch_path,
            on_event=lambda event: self._handle_managed_event(control_id, event),
            on_close=lambda: self._handle_managed_close(control_id),
        )
        self._managed_runtime[control_id] = runtime
        self._managed_sessions[control_id] = bridge
        try:
            await bridge.start()
        except Exception:
            self._managed_runtime.pop(control_id, None)
            self._managed_sessions.pop(control_id, None)
            raise
        return {"ok": True, "proxy_url": bridge.proxy_url}

    def status_payload(self) -> dict[str, object]:
        current = self.store.load()
        snapshot = self._snapshot()
        return {
            "agent_running": True,
            "buddy_connected": self._ble_connected,
            "paired_device_id": current.paired_device_id,
            "paired_device_name": current.paired_device_name,
            "socket_path": str(self.socket_path),
            "snapshot": snapshot.as_ble_payload(),
            "sessions": [session.as_dict() for session in self.catalog.sessions(now=self.clock())],
        }

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        response: dict[str, object]
        try:
            raw = await reader.readline()
            if not raw:
                return
            payload = json.loads(raw.decode("utf-8"))
            response = await self._handle_command(payload)
        except Exception as exc:
            response = {"ok": False, "error": str(exc)}
        writer.write((json.dumps(response, separators=(",", ":")) + "\n").encode("utf-8"))
        await writer.drain()
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()

    async def _handle_command(self, payload: dict[str, object]) -> dict[str, object]:
        command = str(payload.get("cmd", ""))
        if command == "ping":
            return {"ok": True}
        if command == "status":
            return {"ok": True, "state": self.status_payload()}
        if command == "sessions":
            return {"ok": True, "sessions": self.status_payload()["sessions"]}
        if command == "launch":
            workdir = Path(str(payload.get("workdir", ""))).expanduser()
            return await self.launch(workdir)
        if command == "stop":
            self._stopped.set()
            return {"ok": True}
        raise AgentClientError("Unknown agent command: {}".format(command))

    async def _readonly_loop(self) -> None:
        while not self._stopped.is_set():
            if self._watcher is not None:
                readonly = self._watcher.poll(now=self.clock())
                self.catalog.replace_readonly(readonly)
                await self._publish_state()
            await asyncio.sleep(self.readonly_poll_interval)

    async def _ble_loop(self) -> None:
        while not self._stopped.is_set():
            current = self.store.load()
            paired_device_id = current.paired_device_id
            paired_device_name = current.paired_device_name
            if not paired_device_id:
                self._ble_connected = False
                await asyncio.sleep(self.reconnect_interval)
                continue
            if self._ble is None or self._ble.device_id != paired_device_id:
                if self._ble is not None:
                    with contextlib.suppress(Exception):
                        await self._ble.disconnect()
                self._ble = self._ble_factory(
                    paired_device_id,
                    device_name=paired_device_name,
                    on_permission=self._handle_device_permission,
                )
                self._ble_connected = False
            if not self._ble_connected:
                try:
                    await self._ble.connect()
                    self._ble_connected = True
                    await self._publish_state(force=True)
                except Exception:
                    self._ble_connected = False
                    if self._ble is not None:
                        with contextlib.suppress(Exception):
                            await self._ble.disconnect()
                    self._ble = None
            await asyncio.sleep(self.reconnect_interval)

    async def _keepalive_loop(self) -> None:
        while not self._stopped.is_set():
            await asyncio.sleep(self.keepalive_interval)
            await self._publish_state(force=True)

    async def _start_account_usage_monitor(self) -> None:
        current = self.store.load()
        if not current.real_codex_path:
            return
        monitor = self._account_usage_monitor_factory(
            codex_path=current.real_codex_path,
            codex_launch_path=current.codex_launch_path,
            on_usage=self._handle_account_usage,
        )
        self._account_usage_monitor = monitor
        try:
            await monitor.start()
        except Exception:
            self._account_usage_monitor = None
            with contextlib.suppress(Exception):
                await monitor.stop()

    async def _handle_account_usage(self, usage: Optional[UsageDisplay]) -> None:
        self._usage = usage
        self._usage_is_known = True
        await self._publish_state()

    async def _handle_managed_event(self, control_id: str, event: object) -> None:
        runtime = self._managed_runtime[control_id]
        previous_session_id = runtime.session_id
        runtime.apply(event, now=self.clock())
        record = runtime.to_record()
        if previous_session_id and record is not None and previous_session_id != record.session_id:
            self.catalog.remove(previous_session_id)
        if record is not None:
            self.catalog.upsert(record)
        if isinstance(event, ApprovalRequest):
            self._request_to_control[event.request_id] = control_id
        elif isinstance(event, ApprovalRequestResolved):
            self._request_to_control.pop(event.request_id, None)
            self.catalog.resolve_prompt(event.request_id)
            if record is not None:
                self.catalog.upsert(runtime.to_record())
        await self._publish_state()

    async def _handle_managed_close(self, control_id: str) -> None:
        runtime = self._managed_runtime.get(control_id)
        if runtime is not None:
            runtime.close(now=self.clock())
            record = runtime.to_record()
            if record is not None:
                self.catalog.upsert(record)
        self._managed_sessions.pop(control_id, None)
        await self._publish_state()

    async def _handle_device_permission(self, request_id: str, decision: str) -> None:
        control_id = self._request_to_control.get(str(request_id))
        if control_id is None:
            return
        bridge = self._managed_sessions.get(control_id)
        if bridge is None:
            return
        await bridge.respond_to_device_approval(request_id, decision)

    async def _publish_state(self, *, force: bool = False) -> None:
        snapshot = self._snapshot()
        payload = snapshot.as_ble_payload()
        if force or payload != self._last_payload:
            if not self._ota_session_active:
                self._last_payload = payload
            if not self._ota_session_active and self._ble is not None and self._ble_connected:
                try:
                    await self._ble.send_snapshot(snapshot)
                except Exception:
                    self._ble_connected = False
                    with contextlib.suppress(Exception):
                        await self._ble.disconnect()
        self._persist(snapshot, agent_running=True)

    def _snapshot(self):
        return replace(
            self.catalog.snapshot(now=self.clock()),
            usage=self._usage,
            usage_is_known=self._usage_is_known,
        )

    def _persist(self, snapshot: Any, *, agent_running: bool) -> None:
        current = self.store.load()
        sessions = [session.as_dict() for session in self.catalog.sessions(now=self.clock())]
        active_thread_id = sessions[0]["session_id"] if sessions else None
        self.store.save(
            PersistedState(
                paired_device_id=current.paired_device_id,
                paired_device_name=current.paired_device_name,
                tokens_today=snapshot.tokens_today,
                tokens_date=current.tokens_date,
                tokens_total=snapshot.tokens,
                active_thread_id=active_thread_id,
                buddy_connected=self._ble_connected,
                last_msg=snapshot.msg,
                snapshot=snapshot.as_ble_payload(),
                sessions=sessions,
                agent_running=agent_running,
                setup_version=current.setup_version,
                real_codex_path=current.real_codex_path,
                codex_launch_path=current.codex_launch_path,
                helper_app_path=current.helper_app_path,
                shim_dir=current.shim_dir,
                shell_integrated=current.shell_integrated,
                service_installed=current.service_installed,
            )
        )


async def wait_for_agent(socket_path: Path, *, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    client = AgentClient(socket_path)
    while time.monotonic() < deadline:
        try:
            await client.request({"cmd": "ping"})
            return
        except AgentClientError:
            await asyncio.sleep(0.1)
    raise AgentClientError("Timed out waiting for buddy agent")


def spawn_agent_process(state_path: Path) -> None:
    command = [
        sys.executable,
        "-m",
        "codex_buddy",
        "--state-path",
        str(state_path),
        "agent",
    ]
    subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
