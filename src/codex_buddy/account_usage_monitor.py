"""Credential-safe monitor for the signed-in Codex account's rate limits."""

from __future__ import annotations

import asyncio
import contextlib
import inspect
import json
import time
from collections.abc import Awaitable, Callable, Mapping
from typing import Any, Optional

import websockets

from .bridge import _free_port, _terminate_process_group, start_loopback_app_server, wait_for_loopback_app_server_ready
from .usage_limits import USAGE_LIMITS_FRESHNESS_SECONDS, UsageDisplay, UsageLimits


RATE_LIMIT_REFRESH_SECONDS = 5 * 60
RECONNECT_BACKOFF_INITIAL_SECONDS = 1.0
RECONNECT_BACKOFF_MAX_SECONDS = 30.0
_EXPIRY_GRACE_SECONDS = 0.001

UsageCallback = Callable[[Optional[UsageDisplay]], Optional[Awaitable[None]]]
ThreadIdsCallback = Callable[[frozenset[str]], Optional[Awaitable[None]]]
AppServerStart = Callable[[str, str, int], object]
ReadyWaiter = Callable[[int], Awaitable[None]]
WebSocketConnect = Callable[[str], Any]
ProcessTerminator = Callable[[object], None]
ExpirySleep = Callable[[float], Awaitable[None]]


class AccountUsageMonitor:
    """Read and safely reduce official Codex account rate-limit snapshots."""

    def __init__(
        self,
        *,
        codex_path: str,
        on_usage: UsageCallback,
        on_thread_ids: Optional[ThreadIdsCallback] = None,
        codex_launch_path: str = "",
        refresh_interval_seconds: float = RATE_LIMIT_REFRESH_SECONDS,
        app_server_start: Optional[AppServerStart] = None,
        wait_until_ready: Optional[ReadyWaiter] = None,
        websocket_connect: Optional[WebSocketConnect] = None,
        terminate_process: Optional[ProcessTerminator] = None,
        now: Callable[[], float] = time.time,
        expiry_sleep: Optional[ExpirySleep] = None,
    ) -> None:
        if not codex_path:
            raise ValueError("A resolved Codex executable path is required")
        if not 0 < refresh_interval_seconds <= USAGE_LIMITS_FRESHNESS_SECONDS:
            raise ValueError("refresh_interval_seconds must stay within usage freshness")
        self.codex_path = codex_path
        self.codex_launch_path = codex_launch_path
        self.on_usage = on_usage
        self.on_thread_ids = on_thread_ids
        self.refresh_interval_seconds = refresh_interval_seconds
        self._app_server_start = app_server_start or self._start_app_server
        self._wait_until_ready = wait_until_ready or self._wait_for_ready
        self._websocket_connect = websocket_connect or websockets.connect
        self._terminate_process = terminate_process or _terminate_process_group
        self._now = now
        self._expiry_sleep = expiry_sleep or asyncio.sleep
        self._port = _free_port()
        self._websocket_url = f"ws://127.0.0.1:{self._port}"
        self._process: Optional[object] = None
        self._start_task: Optional[asyncio.Task[None]] = None
        self._start_waiter_counts: dict[asyncio.Task[None], int] = {}
        self._task: Optional[asyncio.Task[None]] = None
        self._expiry_task: Optional[asyncio.Task[None]] = None
        self._lifecycle_lock = asyncio.Lock()
        self._lifecycle_generation = 0
        self._stopping = False
        self._limits = UsageLimits(primary=None, secondary=None, observed_at=None)
        self._request_id = 0
        self._read_request_ids: set[int] = set()
        self._thread_list_request_ids: set[int] = set()

    def start(self) -> Awaitable[None]:
        lifecycle_generation = self._lifecycle_generation
        return self._start_for_lifecycle(lifecycle_generation)

    async def _start_for_lifecycle(self, lifecycle_generation: int) -> None:
        async with self._lifecycle_lock:
            if lifecycle_generation != self._lifecycle_generation:
                return
            if self._task is not None:
                return
            start_task = self._start_task
            if start_task is None:
                self._stopping = False
                start_task = asyncio.create_task(self._start_monitor(), name="code-buddy-account-usage-start")
                self._start_task = start_task
            self._start_waiter_counts[start_task] = self._start_waiter_counts.get(start_task, 0) + 1
        cancelled = False
        try:
            await asyncio.shield(start_task)
        except asyncio.CancelledError:
            cancelled = True
            raise
        finally:
            async with self._lifecycle_lock:
                remaining_waiters = self._start_waiter_counts[start_task] - 1
                if remaining_waiters:
                    self._start_waiter_counts[start_task] = remaining_waiters
                else:
                    del self._start_waiter_counts[start_task]
                should_cancel_start = (
                    cancelled
                    and remaining_waiters == 0
                    and self._start_task is start_task
                    and not start_task.done()
                )
                if should_cancel_start:
                    start_task.cancel()
            if should_cancel_start:
                with contextlib.suppress(asyncio.CancelledError):
                    await start_task

    async def _start_monitor(self) -> None:
        try:
            await self._start_owned_app_server()
            self._task = asyncio.create_task(self._run(), name="code-buddy-account-usage")
        finally:
            self._start_task = None

    async def _start_owned_app_server(self) -> None:
        self._process = self._app_server_start(self.codex_path, self.codex_launch_path, self._port)
        try:
            await self._wait_until_ready(self._port)
        except BaseException:
            self._stop_process()
            raise

    async def stop(self) -> None:
        async with self._lifecycle_lock:
            self._lifecycle_generation += 1
            self._stopping = True
            start_task = self._start_task
            if start_task is not None:
                start_task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await start_task
            task = self._task
            self._task = None
            if task is not None:
                task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await task
            await self._stop_expiry_task()
            self._stop_process()

    def _start_app_server(self, codex_path: str, codex_launch_path: str, port: int) -> object:
        return start_loopback_app_server(
            codex_path=codex_path,
            codex_launch_path=codex_launch_path,
            port=port,
        )

    async def _wait_for_ready(self, port: int) -> None:
        await wait_for_loopback_app_server_ready(port)

    def _stop_process(self) -> None:
        if self._process is not None:
            self._terminate_process(self._process)
            self._process = None

    def _process_has_exited(self) -> bool:
        if self._process is None:
            return True
        poll = getattr(self._process, "poll", None)
        return callable(poll) and poll() is not None

    async def _ensure_owned_app_server_is_running(self) -> None:
        if not self._process_has_exited():
            return
        self._stop_process()
        await self._start_owned_app_server()

    async def _run(self) -> None:
        backoff = RECONNECT_BACKOFF_INITIAL_SECONDS
        while not self._stopping:
            try:
                await self._ensure_owned_app_server_is_running()
                async with self._websocket_connect(self._websocket_url) as websocket:
                    await self._initialize(websocket)
                    backoff = RECONNECT_BACKOFF_INITIAL_SECONDS
                    await self._receive_updates(websocket)
            except asyncio.CancelledError:
                raise
            except Exception:
                if self._stopping:
                    return
                await asyncio.sleep(backoff)
                backoff = min(RECONNECT_BACKOFF_MAX_SECONDS, backoff * 2)

    async def _initialize(self, websocket: Any) -> None:
        initialize_id = await self._send_request(
            websocket,
            "initialize",
            {
                "clientInfo": {
                    "name": "code_buddy",
                    "title": "Code Buddy",
                    "version": "0.1.4",
                }
            },
        )
        await self._wait_for_response(websocket, initialize_id)
        await self._send_notification(websocket, "initialized")
        await self._request_rate_limits(websocket)
        await self._request_thread_list(websocket)

    async def _receive_updates(self, websocket: Any) -> None:
        while not self._stopping:
            try:
                raw = await asyncio.wait_for(websocket.recv(), timeout=self.refresh_interval_seconds)
            except asyncio.TimeoutError:
                await self._request_rate_limits(websocket)
                await self._request_thread_list(websocket)
                continue
            await self._handle_message(raw)

    async def _wait_for_response(self, websocket: Any, request_id: int) -> None:
        while True:
            message = self._decode_message(await websocket.recv())
            if message is None:
                continue
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError("Codex app-server initialization failed")
                return
            await self._handle_notification(message)

    async def _request_rate_limits(self, websocket: Any) -> None:
        request_id = await self._send_request(websocket, "account/rateLimits/read")
        self._read_request_ids.add(request_id)

    async def _request_thread_list(self, websocket: Any) -> None:
        request_id = await self._send_request(
            websocket,
            "thread/list",
            {"archived": False, "limit": 1000, "useStateDbOnly": True},
        )
        self._thread_list_request_ids.add(request_id)

    async def _send_request(self, websocket: Any, method: str, params: Optional[dict[str, object]] = None) -> int:
        self._request_id += 1
        payload: dict[str, object] = {"id": self._request_id, "method": method}
        if params is not None:
            payload["params"] = params
        await websocket.send(json.dumps(payload))
        return self._request_id

    async def _send_notification(self, websocket: Any, method: str) -> None:
        await websocket.send(json.dumps({"method": method}))

    async def _handle_message(self, raw: object) -> None:
        message = self._decode_message(raw)
        if message is None:
            return
        request_id = message.get("id")
        if isinstance(request_id, int) and request_id in self._read_request_ids:
            self._read_request_ids.remove(request_id)
            if "result" in message:
                self._limits = UsageLimits.from_read_result(message["result"], observed_at=self._now())
                await self._publish_display()
            return
        if isinstance(request_id, int) and request_id in self._thread_list_request_ids:
            self._thread_list_request_ids.remove(request_id)
            result = message.get("result")
            if not isinstance(result, Mapping):
                return
            data = result.get("data")
            if not isinstance(data, list):
                return
            thread_ids: set[str] = set()
            for item in data:
                if not isinstance(item, Mapping):
                    return
                thread_id = item.get("id")
                if not isinstance(thread_id, str):
                    return
                thread_ids.add(thread_id)
            await self._notify_thread_ids(frozenset(thread_ids))
            return
        await self._handle_notification(message)

    async def _handle_notification(self, message: Mapping[str, object]) -> None:
        if message.get("method") != "account/rateLimits/updated":
            return
        params = message.get("params")
        self._limits = self._limits.merge_update(params, observed_at=self._now())
        await self._publish_display()

    async def _publish_display(self) -> None:
        display = self._limits.display_windows(now=self._now())
        self._schedule_expiry(display)
        await self._notify_usage(display)

    async def _notify_usage(self, display: Optional[UsageDisplay]) -> None:
        result = self.on_usage(display)
        if inspect.isawaitable(result):
            await result

    async def _notify_thread_ids(self, thread_ids: frozenset[str]) -> None:
        if self.on_thread_ids is None:
            return
        result = self.on_thread_ids(thread_ids)
        if inspect.isawaitable(result):
            await result

    def _schedule_expiry(self, display: Optional[UsageDisplay]) -> None:
        task = self._expiry_task
        if task is not None:
            task.cancel()
        self._expiry_task = None
        observed_at = self._limits.observed_at
        if display is None or observed_at is None:
            return
        delay = max(0.0, observed_at + USAGE_LIMITS_FRESHNESS_SECONDS - self._now())
        self._expiry_task = asyncio.create_task(
            self._expire_display_after(observed_at, delay + _EXPIRY_GRACE_SECONDS),
            name="code-buddy-account-usage-expiry",
        )

    async def _expire_display_after(self, observed_at: float, delay: float) -> None:
        await self._expiry_sleep(delay)
        if self._stopping or self._limits.observed_at != observed_at:
            return
        if self._limits.display_windows(now=self._now()) is None:
            await self._notify_usage(None)

    async def _stop_expiry_task(self) -> None:
        task = self._expiry_task
        self._expiry_task = None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task

    @staticmethod
    def _decode_message(raw: object) -> Optional[Mapping[str, object]]:
        if not isinstance(raw, str):
            return None
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            return None
        return parsed if isinstance(parsed, Mapping) else None
