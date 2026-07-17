import asyncio
import ast
import contextlib
import inspect
import json
import subprocess
import sys
from pathlib import Path
from typing import Optional

import pytest

from codex_buddy import __version__
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
        self.exit_code = None

    def poll(self):
        return self.exit_code


class _FakeWebSocket:
    def __init__(self, incoming) -> None:
        self.sent: list[dict[str, object]] = []
        self.incoming = incoming
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
    def __init__(self, now_value: float, release_sleep) -> None:
        self.now_value = now_value
        self.sleep_delays: list[float] = []
        self.release_sleep = release_sleep

    def now(self) -> float:
        return self.now_value

    async def sleep(self, delay: float) -> None:
        self.sleep_delays.append(delay)
        await self.release_sleep.wait()


def test_fake_async_primitives_are_not_created_by_sync_test_helpers():
    source = Path(__file__).read_text(encoding="utf-8")
    tree = ast.parse(source)
    helper_classes = {
        node.name: node
        for node in ast.walk(tree)
        if isinstance(node, ast.ClassDef) and node.name in {"_FakeWebSocket", "_FakeClock"}
    }

    for helper in helper_classes.values():
        assert not any(
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "asyncio"
            and node.func.attr in {"Event", "Queue"}
            for node in ast.walk(helper)
        )


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


def test_monitor_cancellation_during_readiness_wait_terminates_its_owned_app_server():
    process = _FakeProcess()
    terminated: list[_FakeProcess] = []

    async def exercise() -> None:
        readiness_started = asyncio.Event()

        async def wait_until_ready(_: int) -> None:
            readiness_started.set()
            await asyncio.Future()

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=lambda *_: process,
            wait_until_ready=wait_until_ready,
            terminate_process=terminated.append,
        )
        start_task = asyncio.create_task(monitor.start())
        await readiness_started.wait()
        start_task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await start_task

    asyncio.run(exercise())

    assert terminated == [process]


def test_monitor_concurrent_starts_share_one_readiness_gate_and_cleanup_one_process():
    launches: list[_FakeProcess] = []
    terminated: list[_FakeProcess] = []

    def launch(*_) -> _FakeProcess:
        process = _FakeProcess()
        launches.append(process)
        return process

    async def exercise() -> None:
        readiness_started = asyncio.Event()
        release_readiness = asyncio.Event()

        async def wait_until_ready(_: int) -> None:
            readiness_started.set()
            await release_readiness.wait()

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            terminate_process=terminated.append,
        )
        first_start = asyncio.create_task(monitor.start())
        await readiness_started.wait()
        second_start = asyncio.create_task(monitor.start())
        await asyncio.sleep(0)

        release_readiness.set()
        await asyncio.gather(first_start, second_start)
        await monitor.stop()

    asyncio.run(exercise())

    assert len(launches) == 1
    assert terminated == launches


def test_monitor_cancelling_one_concurrent_start_does_not_cancel_the_shared_startup():
    launches: list[_FakeProcess] = []
    terminated: list[_FakeProcess] = []

    def launch(*_) -> _FakeProcess:
        process = _FakeProcess()
        launches.append(process)
        return process

    async def exercise() -> None:
        readiness_started = asyncio.Event()
        release_readiness = asyncio.Event()
        socket = _FakeWebSocket(asyncio.Queue())

        async def wait_until_ready(_: int) -> None:
            readiness_started.set()
            await release_readiness.wait()

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            websocket_connect=lambda _: socket,
            terminate_process=terminated.append,
        )
        cancelled_start = asyncio.create_task(monitor.start())
        await readiness_started.wait()
        completing_start = asyncio.create_task(monitor.start())
        await asyncio.sleep(0)

        cancelled_start.cancel()
        with pytest.raises(asyncio.CancelledError):
            await cancelled_start

        assert not completing_start.done()
        assert terminated == []

        release_readiness.set()
        await completing_start
        assert monitor._task is not None
        assert not monitor._task.done()

        await monitor.stop()

    asyncio.run(exercise())

    assert len(launches) == 1
    assert terminated == launches


def test_monitor_stop_prevents_a_previously_scheduled_start_from_launching():
    launches: list[_FakeProcess] = []
    terminated: list[_FakeProcess] = []

    def launch(*_) -> _FakeProcess:
        process = _FakeProcess()
        launches.append(process)
        return process

    async def exercise() -> None:
        async def wait_until_ready(_: int) -> None:
            return None

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            terminate_process=terminated.append,
        )
        pending_start = asyncio.create_task(monitor.start())

        await monitor.stop()
        await pending_start

    asyncio.run(exercise())

    assert launches == []
    assert terminated == []


def test_monitor_start_waits_for_an_in_progress_stop_to_finish_cleanup():
    launches: list[_FakeProcess] = []
    terminated: list[_FakeProcess] = []

    def launch(*_) -> _FakeProcess:
        process = _FakeProcess()
        launches.append(process)
        return process

    async def exercise() -> None:
        expiry_cancelled = asyncio.Event()
        release_expiry = asyncio.Event()

        async def wait_until_ready(_: int) -> None:
            return None

        async def expiry() -> None:
            try:
                await asyncio.Future()
            except asyncio.CancelledError:
                expiry_cancelled.set()
                await release_expiry.wait()
                raise

        socket = _FakeWebSocket(asyncio.Queue())
        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            websocket_connect=lambda _: socket,
            terminate_process=terminated.append,
        )
        await monitor.start()
        monitor._expiry_task = asyncio.create_task(expiry())
        await asyncio.sleep(0)

        stop_task = asyncio.create_task(monitor.stop())
        await expiry_cancelled.wait()
        restart_task = asyncio.create_task(monitor.start())
        await asyncio.sleep(0)

        release_expiry.set()
        await stop_task
        await restart_task
        await monitor.stop()

    asyncio.run(exercise())

    assert len(launches) == 2
    assert terminated == launches


def test_monitor_cancellation_during_readiness_wait_stops_its_start_new_session_child():
    async def exercise() -> int:
        readiness_started = asyncio.Event()
        process: Optional[subprocess.Popen] = None

        def launch(*_) -> subprocess.Popen:
            nonlocal process
            process = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(60)"],
                start_new_session=True,
            )
            return process

        async def wait_until_ready(_: int) -> None:
            readiness_started.set()
            await asyncio.Future()

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
        )
        start_task = asyncio.create_task(monitor.start())
        try:
            await readiness_started.wait()
            assert process is not None
            assert process.poll() is None
            start_task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await start_task
            await _wait_for(lambda: process.poll() is not None)
            return process.returncode
        finally:
            if not start_task.done():
                start_task.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await start_task
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=1)

    assert asyncio.run(exercise()) is not None


def test_monitor_restarts_its_owned_app_server_after_process_exit_before_reconnecting(monkeypatch):
    first_process = _FakeProcess()
    second_process = _FakeProcess()
    launches: list[_FakeProcess] = []
    ready_ports: list[int] = []
    terminated: list[_FakeProcess] = []
    connection_attempts: list[object] = []
    original_sleep = asyncio.sleep

    async def no_wait_retry(_: float) -> None:
        await original_sleep(0)

    monkeypatch.setattr(account_usage_monitor.asyncio, "sleep", no_wait_retry)

    def launch(*_) -> _FakeProcess:
        process = [first_process, second_process][len(launches)]
        launches.append(process)
        return process

    async def exercise() -> None:
        socket = _FakeWebSocket(asyncio.Queue())

        async def wait_until_ready(port: int) -> None:
            ready_ports.append(port)

        def connect(_: str):
            connection_attempts.append(None)
            if len(connection_attempts) == 1:
                first_process.exit_code = 1
                raise ConnectionError("transport closed")
            return socket

        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            app_server_start=launch,
            wait_until_ready=wait_until_ready,
            websocket_connect=connect,
            terminate_process=terminated.append,
        )
        await monitor.start()

        await _wait_for(lambda: len(launches) == 2)
        await monitor.stop()

    asyncio.run(exercise())

    assert ready_ports[0] == ready_ports[1]
    assert len(connection_attempts) == 2
    assert terminated == [first_process, second_process]


def test_monitor_withdraws_display_after_freshness_expires_while_reconnecting():
    process = _FakeProcess()
    seen: list[object] = []

    async def exercise() -> None:
        socket = _FakeWebSocket(asyncio.Queue())
        clock = _FakeClock(now_value=100.0, release_sleep=asyncio.Event())

        async def ready(_: int) -> None:
            return None

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
    launches: list[tuple[str, str, int, str]] = []
    terminated: list[_FakeProcess] = []
    seen: list[object] = []

    def launch(codex_path: str, codex_launch_path: str, port: int):
        launches.append((codex_path, codex_launch_path, port, f"ws://127.0.0.1:{port}"))
        return process

    async def exercise() -> None:
        socket = _FakeWebSocket(asyncio.Queue())

        async def wait_until_ready(port: int) -> None:
            assert port == launches[0][2]

        def connect(url: str):
            assert url == launches[0][3]
            return socket

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
            "thread/list",
        ]
        assert socket.sent[0]["params"]["clientInfo"]["version"] == __version__
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
        return socket.closed

    socket_closed = asyncio.run(exercise())

    assert terminated == [process]
    assert socket_closed is True


def test_monitor_publishes_current_non_archived_thread_ids() -> None:
    seen: list[frozenset[str]] = []

    async def exercise() -> None:
        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=lambda _: None,
            on_thread_ids=seen.append,
        )
        monitor._thread_list_request_ids.add(7)
        await monitor._handle_message(
            json.dumps(
                {
                    "id": 7,
                    "result": {
                        "data": [
                            {"id": "thread-visible-a"},
                            {"id": "thread-visible-b"},
                        ],
                        "nextCursor": None,
                    },
                }
            )
        )

    asyncio.run(exercise())

    assert seen == [frozenset({"thread-visible-a", "thread-visible-b"})]


def test_monitor_publishes_the_current_week_only_codex_bucket_shape():
    seen: list[object] = []

    async def exercise() -> None:
        monitor = AccountUsageMonitor(
            codex_path="/usr/local/bin/codex-real",
            on_usage=seen.append,
            now=lambda: 100.0,
        )
        monitor._read_request_ids.add(7)
        await monitor._handle_message(
            json.dumps(
                {
                    "id": 7,
                    "result": {
                        "rateLimitsByLimitId": {
                            "codex": {
                                "primary": {
                                    "usedPercent": 1,
                                    "windowDurationMins": 10_080,
                                    "resetsAt": 1_700_100_000,
                                },
                                "secondary": None,
                            },
                            "codex_bengalfox": _rate_limits(80, 70)["rateLimits"],
                        }
                    },
                }
            )
        )
        await monitor._stop_expiry_task()

    asyncio.run(exercise())

    assert seen == [UsageDisplay(seven_day_remaining=99)]


def test_monitor_cleanup_cancels_a_waiting_receive_loop_and_never_uses_auth_arguments():
    process = _FakeProcess()
    launch_calls: list[tuple[tuple[object, ...], dict[str, object]]] = []
    terminated: list[_FakeProcess] = []

    def launch(*args, **kwargs):
        launch_calls.append((args, kwargs))
        return process

    async def ready(_: int) -> None:
        return None

    async def exercise() -> None:
        socket = _FakeWebSocket(asyncio.Queue())
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
        return socket.closed

    socket_closed = asyncio.run(exercise())

    launch_args, launch_kwargs = launch_calls[0]
    assert launch_args[0] == "/private/var/code-buddy/bin/codex-real"
    assert "auth" not in " ".join(map(str, launch_args)).lower()
    assert "token" not in " ".join(map(str, launch_args)).lower()
    assert launch_kwargs == {}
    assert terminated == [process]
    assert socket_closed is True
