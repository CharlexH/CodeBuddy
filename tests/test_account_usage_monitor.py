import asyncio
import ast
import inspect
import json
from pathlib import Path

import pytest

from codex_buddy import account_usage_monitor
from codex_buddy.account_usage_monitor import AccountUsageMonitor
from codex_buddy.usage_limits import USAGE_LIMITS_FRESHNESS_SECONDS, UsageDisplay


def _rate_limits(primary_used: object, secondary_used: object) -> dict[str, object]:
    return {
        "rateLimits": {
            "primary": {
                "usedPercent": primary_used,
                "windowDurationMins": 300,
                "resetsAt": 1_700_000_000,
            },
            "secondary": {
                "usedPercent": secondary_used,
                "windowDurationMins": 10_080,
                "resetsAt": 1_700_100_000,
            },
        }
    }


async def _wait_for(predicate, *, timeout: float = 1.0) -> None:
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        if asyncio.get_running_loop().time() >= deadline:
            raise AssertionError("condition was not reached before timeout")
        await asyncio.sleep(0)


class _FakeProcess:
    def __init__(self) -> None:
        self.terminated = False


class _FakeWebSocket:
    def __init__(self) -> None:
        self.sent: list[dict[str, object]] = []
        self.incoming: asyncio.Queue[object] = asyncio.Queue()
        self.closed = False

    async def __aenter__(self):
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.closed = True

    async def send(self, raw: str) -> None:
        self.sent.append(json.loads(raw))

    async def recv(self) -> str:
        item = await self.incoming.get()
        if isinstance(item, BaseException):
            raise item
        return json.dumps(item)

    async def deliver(self, message: dict[str, object]) -> None:
        await self.incoming.put(message)


class _FakeClock:
    def __init__(self, now_value: float) -> None:
        self.now_value = now_value
        self.sleep_delays: list[float] = []
        self.release_sleep = asyncio.Event()

    def now(self) -> float:
        return self.now_value

    async def sleep(self, delay: float) -> None:
        self.sleep_delays.append(delay)
        await self.release_sleep.wait()


def test_monitor_module_has_no_runtime_union_annotation_that_breaks_python_39_imports():
    source = Path(inspect.getfile(account_usage_monitor)).read_text(encoding="utf-8")
    tree = ast.parse(source)

    assert not any(
        isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitOr)
        for node in ast.walk(tree)
    )


def test_monitor_rejects_a_refresh_interval_that_would_outlive_a_display_snapshot():
    with pytest.raises(ValueError, match="freshness"):
        AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            refresh_interval_seconds=USAGE_LIMITS_FRESHNESS_SECONDS + 1,
        )


def test_monitor_withdraws_display_after_freshness_expires_while_reconnecting():
    process = _FakeProcess()
    socket = _FakeWebSocket()
    clock = _FakeClock(now_value=100.0)
    seen: list[object] = []

    async def ready(_: int) -> None:
        return None

    async def exercise() -> None:
        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=seen.append,
            app_server_start=lambda *_: process,
            wait_until_ready=ready,
            websocket_connect=lambda _: socket,
            terminate_process=lambda _: None,
            now=clock.now,
            expiry_sleep=clock.sleep,
        )
        await socket.deliver({"id": 1, "result": {}})
        await socket.deliver({"id": 2, "result": _rate_limits(28, 9)})
        await socket.deliver(ConnectionError("transport closed"))

        await monitor.start()
        await _wait_for(lambda: seen == [UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)])
        await _wait_for(lambda: len(clock.sleep_delays) == 1)

        clock.now_value += USAGE_LIMITS_FRESHNESS_SECONDS + 1
        clock.release_sleep.set()
        await _wait_for(lambda: seen[-1:] == [None])
        await monitor.stop()

    asyncio.run(exercise())


def test_monitor_reads_rate_limits_merges_sparse_updates_and_exposes_only_display_values():
    process = _FakeProcess()
    socket = _FakeWebSocket()
    launches: list[tuple[str, str, int, str]] = []
    terminated: list[_FakeProcess] = []
    seen: list[object] = []

    def launch(codex_path: str, codex_launch_path: str, port: int):
        launches.append((codex_path, codex_launch_path, port, f"ws://127.0.0.1:{port}"))
        return process

    async def wait_until_ready(port: int) -> None:
        assert port == launches[0][2]

    def connect(url: str):
        assert url == launches[0][3]
        return socket

    async def exercise() -> None:
        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            codex_launch_path="/opt/homebrew/bin",
            on_usage=seen.append,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            websocket_connect=connect,
            terminate_process=terminated.append,
        )
        await socket.deliver({"id": 1, "result": {}})
        await socket.deliver({"id": 2, "result": _rate_limits(28, 9)})

        await monitor.start()
        await _wait_for(lambda: len(seen) == 1)

        assert launches[0][:2] == ("/usr/local/bin/codex-real", "/opt/homebrew/bin")
        assert [message.get("method") for message in socket.sent] == [
            "initialize",
            "initialized",
            "account/rateLimits/read",
        ]
        assert seen == [UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)]
        assert all(isinstance(value, UsageDisplay) or value is None for value in seen)

        await socket.deliver(
            {
                "method": "account/rateLimits/updated",
                "params": {"rateLimits": {"primary": {"usedPercent": 40}}},
            }
        )
        await _wait_for(lambda: len(seen) == 2)
        assert seen[-1] == UsageDisplay(five_hour_remaining=60, seven_day_remaining=91)

        await monitor.stop()

    asyncio.run(exercise())

    assert terminated == [process]
    assert socket.closed is True


def test_monitor_cleanup_cancels_a_waiting_receive_loop_and_never_uses_auth_arguments():
    process = _FakeProcess()
    socket = _FakeWebSocket()
    launch_calls: list[tuple[tuple[object, ...], dict[str, object]]] = []
    terminated: list[_FakeProcess] = []

    def launch(*args, **kwargs):
        launch_calls.append((args, kwargs))
        return process

    async def ready(_: int) -> None:
        return None

    async def exercise() -> None:
        monitor = AccountUsageMonitor(
            codex_path="/private/var/code-buddy/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=ready,
            websocket_connect=lambda _: socket,
            terminate_process=terminated.append,
        )
        await socket.deliver({"id": 1, "result": {}})
        await monitor.start()
        await _wait_for(lambda: len(socket.sent) >= 2)
        await monitor.stop()

    asyncio.run(exercise())

    launch_args, launch_kwargs = launch_calls[0]
    assert launch_args[0] == "/private/var/code-buddy/bin/codex-real"
    assert "auth" not in " ".join(map(str, launch_args)).lower()
    assert "token" not in " ".join(map(str, launch_args)).lower()
    assert launch_kwargs == {}
    assert terminated == [process]
    assert socket.closed is True
