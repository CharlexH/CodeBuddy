from __future__ import annotations

import asyncio
import contextlib
import os
import signal
import socket
import subprocess
import time
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Awaitable, Callable, Optional

from .ble_transport import BleBuddyTransport
from .proxy import ApprovalRequestResolved, CodexEventSource
from .reducer import BuddySnapshot, BuddyStateReducer
from .runtime import state_path as runtime_state_path
from .state_store import BridgeStateStore, PersistedState

_COMMON_CODEX_PATHS = (
    "/usr/local/bin",
    "/opt/homebrew/bin",
    "/Applications/Codex.app/Contents/Resources",
    "/usr/bin",
    "/bin",
    "/usr/sbin",
    "/sbin",
)


def _free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _codex_process_env(codex_path: str, codex_launch_path: str = "") -> dict[str, str]:
    env = os.environ.copy()
    path_entries: list[str] = []

    if codex_path and os.sep in codex_path:
        parent = Path(codex_path).expanduser().parent
        if str(parent) not in {"", "."}:
            _append_path_entries(path_entries, str(parent))

    _append_path_entries(path_entries, codex_launch_path)
    _append_path_entries(path_entries, env.get("PATH", ""))
    _append_path_entries(path_entries, os.pathsep.join(_COMMON_CODEX_PATHS))

    env["PATH"] = os.pathsep.join(path_entries)
    env["CODE_BUDDY_SHIM_ACTIVE"] = "1"
    return env


def _append_path_entries(entries: list[str], path_text: str) -> None:
    for raw_entry in path_text.split(os.pathsep):
        entry = raw_entry.strip()
        if entry and entry not in entries:
            entries.append(entry)


def _terminate_process_group(process: subprocess.Popen) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except OSError:
        process.terminate()
    with contextlib.suppress(subprocess.TimeoutExpired):
        process.wait(timeout=5)
        return
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except OSError:
        process.kill()
    with contextlib.suppress(subprocess.TimeoutExpired):
        process.wait(timeout=5)


def start_loopback_app_server(
    *,
    codex_path: str,
    codex_launch_path: str,
    port: int,
) -> subprocess.Popen:
    return subprocess.Popen(
        [
            codex_path,
            "app-server",
            "--listen",
            f"ws://127.0.0.1:{port}",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        env=_codex_process_env(codex_path, codex_launch_path),
        start_new_session=True,
    )


async def wait_for_loopback_app_server_ready(port: int, *, timeout_seconds: float = 10.0) -> None:
    deadline = time.time() + timeout_seconds
    ready_url = f"http://127.0.0.1:{port}/readyz"
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(ready_url, timeout=0.5) as response:
                if response.status == 200:
                    return
        except Exception:
            await asyncio.sleep(0.2)
    raise RuntimeError("Timed out waiting for codex app-server to become ready")


@dataclass(frozen=True)
class RunConfig:
    workdir: Path
    prompt: Optional[str]
    state_path: Path
    paired_device_id: str
    paired_device_name: Optional[str]
    codex_path: str = "codex"
    codex_launch_path: str = ""


class BridgeController:
    def __init__(self, config: RunConfig) -> None:
        self.config = config
        self.store = BridgeStateStore(config.state_path)
        persisted = self.store.load()
        self.reducer = BuddyStateReducer(tokens=persisted.tokens_total, tokens_today=persisted.tokens_today)
        self.ble = BleBuddyTransport(
            config.paired_device_id,
            device_name=config.paired_device_name,
            on_permission=self._handle_device_permission,
        )
        self.upstream_port = _free_port()
        self.proxy_port = _free_port()
        self.upstream_url = f"ws://127.0.0.1:{self.upstream_port}"
        self.proxy = CodexEventSource(
            upstream_url=self.upstream_url,
            listen_host="127.0.0.1",
            listen_port=self.proxy_port,
            on_event=self._handle_event,
        )
        self._upstream_proc: Optional[subprocess.Popen] = None
        self._active_thread_id: Optional[str] = persisted.active_thread_id
        self._codex_env = _codex_process_env(config.codex_path, config.codex_launch_path)

    async def run(self) -> int:
        await self._start_upstream()
        await self.proxy.start()
        await self.ble.connect()
        await self.ble.send_snapshot(self.reducer.snapshot())
        try:
            return await self._run_codex()
        finally:
            await self.ble.disconnect()
            await self.proxy.stop()
            if self._upstream_proc is not None:
                _terminate_process_group(self._upstream_proc)
                self._upstream_proc = None
            self._persist_snapshot(self.reducer.snapshot(), buddy_connected=False)

    async def _start_upstream(self) -> None:
        self._upstream_proc = start_loopback_app_server(
            codex_path=self.config.codex_path,
            codex_launch_path=self.config.codex_launch_path,
            port=self.upstream_port,
        )
        try:
            await wait_for_loopback_app_server_ready(self.upstream_port)
        except Exception:
            if self._upstream_proc is not None:
                _terminate_process_group(self._upstream_proc)
                self._upstream_proc = None
            raise

    async def _run_codex(self) -> int:
        command = [
            self.config.codex_path,
            "--remote",
            f"ws://127.0.0.1:{self.proxy_port}",
            "-a",
            "untrusted",
            "-C",
            str(self.config.workdir),
        ]
        if self.config.prompt:
            command.append(self.config.prompt)
        process = await asyncio.create_subprocess_exec(
            *command,
            stdin=None,
            stdout=None,
            stderr=None,
            env=self._codex_env,
        )
        return await process.wait()

    async def _handle_event(self, event: object) -> None:
        thread_id = getattr(event, "thread_id", None)
        if thread_id:
            self._active_thread_id = thread_id
        if hasattr(event, "active") and getattr(event, "active") is False:
            event_thread_id = getattr(event, "thread_id", None)
            if event_thread_id and self._active_thread_id == event_thread_id:
                self._active_thread_id = None
        if isinstance(event, ApprovalRequestResolved):
            self.reducer.resolve_approval(event.request_id)
        else:
            self.reducer.apply(event)
        snapshot = self.reducer.snapshot()
        self._persist_snapshot(snapshot, buddy_connected=True)
        await self.ble.send_snapshot(snapshot)

    async def _handle_device_permission(self, request_id: str, decision: str) -> None:
        await self.proxy.respond_to_device_approval(request_id, decision)

    def _persist_snapshot(self, snapshot: BuddySnapshot, *, buddy_connected: bool) -> None:
        current = self.store.load()
        self.store.save(
            PersistedState(
                paired_device_id=current.paired_device_id or self.config.paired_device_id,
                paired_device_name=current.paired_device_name or self.config.paired_device_name,
                tokens_today=snapshot.tokens_today,
                tokens_date=self.store.load().tokens_date or "",
                tokens_total=snapshot.tokens,
                active_thread_id=self._active_thread_id,
                buddy_connected=buddy_connected,
                last_msg=snapshot.msg,
                snapshot=snapshot.as_ble_payload(),
                setup_version=current.setup_version,
                real_codex_path=current.real_codex_path,
                codex_launch_path=current.codex_launch_path,
                helper_app_path=current.helper_app_path,
                shim_dir=current.shim_dir,
                shell_integrated=current.shell_integrated,
                service_installed=current.service_installed,
            )
        )


def default_state_path() -> Path:
    return runtime_state_path()


ManagedEventCallback = Callable[[object], Awaitable[None]]
ManagedCloseCallback = Callable[[], Awaitable[None]]


class ManagedSessionBridge:
    def __init__(
        self,
        *,
        workdir: Path,
        codex_path: str = "codex",
        codex_launch_path: str = "",
        on_event: ManagedEventCallback,
        on_close: Optional[ManagedCloseCallback] = None,
    ) -> None:
        self.workdir = workdir
        self.codex_path = codex_path
        self.codex_launch_path = codex_launch_path
        self._codex_env = _codex_process_env(codex_path, codex_launch_path)
        self.on_event = on_event
        self.on_close = on_close
        self.upstream_port = _free_port()
        self.proxy_port = _free_port()
        self.upstream_url = f"ws://127.0.0.1:{self.upstream_port}"
        self.proxy = CodexEventSource(
            upstream_url=self.upstream_url,
            listen_host="127.0.0.1",
            listen_port=self.proxy_port,
            on_event=self.on_event,
            on_close=self._handle_close,
        )
        self._upstream_proc: Optional[subprocess.Popen] = None

    @property
    def proxy_url(self) -> str:
        return f"ws://127.0.0.1:{self.proxy_port}"

    async def start(self) -> None:
        await self._start_upstream()
        await self.proxy.start()

    async def stop(self) -> None:
        await self.proxy.stop()
        if self._upstream_proc is not None:
            _terminate_process_group(self._upstream_proc)
            self._upstream_proc = None

    async def respond_to_device_approval(self, request_id: str, decision: str) -> None:
        await self.proxy.respond_to_device_approval(request_id, decision)

    async def _start_upstream(self) -> None:
        self._upstream_proc = start_loopback_app_server(
            codex_path=self.codex_path,
            codex_launch_path=self.codex_launch_path,
            port=self.upstream_port,
        )
        try:
            await wait_for_loopback_app_server_ready(self.upstream_port)
        except Exception:
            if self._upstream_proc is not None:
                _terminate_process_group(self._upstream_proc)
                self._upstream_proc = None
            raise

    async def _handle_close(self) -> None:
        if self.on_close is not None:
            await self.on_close()
