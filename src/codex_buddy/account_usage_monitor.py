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

UsageCallback = Callable[[Optional[UsageDisplay]], Optional[Awaitable[None]]]
AppServerStart = Callable[[str, str, int], object]
ReadyWaiter = Callable[[int], Awaitable[None]]
WebSocketConnect = Callable[[str], Any]
ProcessTerminator = Callable[[object], None]


class AccountUsageMonitor:
    """Read and safely reduce official Codex account rate-limit snapshots."""

    def __init__(
        self,
        *,
        codex_path: str,
        on_usage: UsageCallback,
        codex_launch_path: str = "",
        refresh_interval_seconds: float = RATE_LIMIT_REFRESH_SECONDS,
        app_server_start: Optional[AppServerStart] = None,
        wait_until_ready: Optional[ReadyWaiter] = None,
        websocket_connect: Optional[WebSocketConnect] = None,
        terminate_process: Optional[ProcessTerminator] = None,
        now: Callable[[], float] = time.time,
    ) -> None:
        if not codex_path:
            raise ValueError("A resolved Codex executable path is required")
        if not 0 < refresh_interval_seconds <= USAGE_LIMITS_FRESHNESS_SECONDS:
            raise ValueError("refresh_interval_seconds must stay within usage freshness")
        self.codex_path = codex_path
        self.codex_launch_path = codex_launch_path
        self.on_usage = on_usage
        self.refresh_interval_seconds = refresh_interval_seconds
        self._app_server_start = app_server_start or self._start_app_server
        self._wait_until_ready = wait_until_ready or self._wait_for_ready
        self._websocket_connect = websocket_connect or websockets.connect
        self._terminate_process = terminate_process or _terminate_process_group
        self._now = now
        self._port = _free_port()
        self._websocket_url = f"ws://127.0.0.1:{self._port}"
        self._process: Optional[object] = None
        self._task: Optional[asyncio.Task[None]] = None
        self._stopping = False
        self._limits = UsageLimits(primary=None, secondary=None, observed_at=None)
        self._request_id = 0
        self._read_request_ids: set[int] = set()

    async def start(self) -> None:
        if self._task is not None:
            return
        self._stopping = False
        self._process = self._app_server_start(self.codex_path, self.codex_launch_path, self._port)
        try:
            await self._wait_until_ready(self._port)
        except Exception:
            self._stop_process()
            raise
        self._task = asyncio.create_task(self._run(), name="code-buddy-account-usage")

    async def stop(self) -> None:
        self._stopping = True
        task = self._task
        self._task = None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
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

    async def _run(self) -> None:
        backoff = RECONNECT_BACKOFF_INITIAL_SECONDS
        while not self._stopping:
            try:
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

    async def _receive_updates(self, websocket: Any) -> None:
        while not self._stopping:
            try:
                raw = await asyncio.wait_for(websocket.recv(), timeout=self.refresh_interval_seconds)
            except asyncio.TimeoutError:
                await self._request_rate_limits(websocket)
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
        await self._handle_notification(message)

    async def _handle_notification(self, message: Mapping[str, object]) -> None:
        if message.get("method") != "account/rateLimits/updated":
            return
        params = message.get("params")
        self._limits = self._limits.merge_update(params, observed_at=self._now())
        await self._publish_display()

    async def _publish_display(self) -> None:
        result = self.on_usage(self._limits.display_pair(now=self._now()))
        if inspect.isawaitable(result):
            await result

    @staticmethod
    def _decode_message(raw: object) -> Optional[Mapping[str, object]]:
        if not isinstance(raw, str):
            return None
        try:
            parsed = json.loads(raw)
        except json.JSONDecodeError:
            return None
        return parsed if isinstance(parsed, Mapping) else None
