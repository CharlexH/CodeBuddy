import asyncio
import os
from pathlib import Path

import pytest

from codex_buddy import ble_transport
from codex_buddy.ble_transport import (
    BleBuddyTransport,
    DiscoveredBuddy,
    NativeBleHelperSession,
    _HelperProcess,
    _matches_buddy_discovery,
    _terminate_native_helper_processes,
)


class _FakeClient:
    def __init__(self) -> None:
        self.is_connected = True
        self.writes: list[tuple[str, bytes, bool]] = []

    async def write_gatt_char(self, uuid: str, data: bytes, response: bool) -> None:
        self.writes.append((uuid, bytes(data), response))


class _FakeNativeSession:
    def __init__(self) -> None:
        self.connected = False
        self.disconnected = False
        self.writes: list[dict] = []
        self.on_permission = None
        self.notifications = None

    @property
    def is_connected(self) -> bool:
        return self.connected

    async def connect(self) -> None:
        self.connected = True

    async def disconnect(self) -> None:
        self.disconnected = True

    async def write_json(self, payload: dict) -> None:
        self.writes.append(payload)

    async def recv_json(self, *, timeout: float) -> dict:
        if self.notifications is None:
            self.notifications = asyncio.Queue()
        return await asyncio.wait_for(self.notifications.get(), timeout=timeout)

    async def emit_permission(self, request_id: str, decision: str) -> None:
        assert self.on_permission is not None
        await self.on_permission(request_id, decision)


def test_ble_transport_uses_write_with_response_for_snapshot_payloads():
    transport = BleBuddyTransport("device-1", use_native_helper=False)
    fake = _FakeClient()
    transport._client = fake

    asyncio.run(
        transport._send_json(
            {
                "total": 1,
                "running": 0,
                "waiting": 1,
                "msg": "approve: rm /tmp/demo",
                "prompt": {
                    "id": "0",
                    "tool": "Bash",
                    "hint": "rm /tmp/demo",
                },
            }
        )
    )

    assert fake.writes
    assert all(response is True for _, _, response in fake.writes)


def test_ble_transport_native_helper_connect_sends_owner_and_time_sync():
    fake = _FakeNativeSession()

    def factory(*, device_id: str, device_name: str, on_permission):
        assert device_id == "device-1"
        assert device_name == "Codex-1234"
        fake.on_permission = on_permission
        return fake

    previous_user = os.environ.get("USER")
    os.environ["USER"] = "BuddyTester"
    try:
        transport = BleBuddyTransport(
            "device-1",
            device_name="Codex-1234",
            use_native_helper=True,
            native_session_factory=factory,
        )
        asyncio.run(transport.connect())
        asyncio.run(transport.disconnect())
    finally:
        if previous_user is None:
            os.environ.pop("USER", None)
        else:
            os.environ["USER"] = previous_user

    assert fake.connected is True
    assert fake.disconnected is True
    assert fake.writes[0] == {"cmd": "owner", "name": "BuddyTester"}
    assert "time" in fake.writes[1]
    assert len(fake.writes[1]["time"]) == 2


def test_ble_transport_reports_native_disconnect_state():
    fake = _FakeNativeSession()

    def factory(**_):
        return fake

    async def exercise():
        transport = BleBuddyTransport(
            "device-1", use_native_helper=True, native_session_factory=factory
        )
        await transport.connect()
        assert transport.is_connected is True
        fake.connected = False
        assert transport.is_connected is False

    asyncio.run(exercise())


def test_native_helper_open_command_launches_without_focus(tmp_path):
    app_path = tmp_path / "CodeBuddyBLEHelper.app"
    session_dir = tmp_path / "session"

    command = ble_transport._native_helper_open_command(
        app_path=app_path,
        session_dir=session_dir,
        device_id="dev-1",
        device_name="Codex-1234",
    )

    assert command[:4] == ["open", "-g", "-j", "-n"]
    assert command[4:] == [
        str(app_path),
        "--args",
        "--session-dir",
        str(session_dir),
        "--device-id",
        "dev-1",
        "--device-name",
        "Codex-1234",
    ]


def test_ble_transport_native_helper_forwards_permission_events():
    fake = _FakeNativeSession()
    approvals: list[tuple[str, str]] = []

    async def on_permission(request_id: str, decision: str) -> None:
        approvals.append((request_id, decision))

    def factory(*, device_id: str, device_name: str, on_permission):
        assert device_id == "device-1"
        assert device_name == "Codex-1234"
        fake.on_permission = on_permission
        return fake

    transport = BleBuddyTransport(
        "device-1",
        device_name="Codex-1234",
        on_permission=on_permission,
        use_native_helper=True,
        native_session_factory=factory,
    )

    asyncio.run(transport.connect())
    asyncio.run(fake.emit_permission("req-1", "deny"))

    assert approvals == [("req-1", "deny")]


def test_native_helper_session_queues_bounded_json_notifications():
    async def exercise():
        session = NativeBleHelperSession(
            device_id="dev-1", device_name="Codex-1234", on_permission=None
        )
        await session._handle_event(
            {
                "event": "notification",
                "line": '{"cmd":"ota_status","nonce":"abc","phase":"authenticated"}',
            }
        )
        assert await session.recv_json(timeout=0.1) == {
            "cmd": "ota_status",
            "nonce": "abc",
            "phase": "authenticated",
        }

        await session._handle_event(
            {"event": "notification", "line": "{" + "x" * 2048 + "}"}
        )
        with pytest.raises(asyncio.TimeoutError):
            await session.recv_json(timeout=0.01)

    asyncio.run(exercise())


def test_ble_transport_sends_and_receives_application_json_without_regressing_permissions():
    fake = _FakeNativeSession()
    approvals: list[tuple[str, str]] = []

    async def on_permission(request_id: str, decision: str) -> None:
        approvals.append((request_id, decision))

    def factory(*, device_id: str, device_name: str, on_permission):
        fake.on_permission = on_permission
        return fake

    async def exercise():
        transport = BleBuddyTransport(
            "device-1",
            device_name="Codex-1234",
            on_permission=on_permission,
            use_native_helper=True,
            native_session_factory=factory,
        )
        await transport.connect()
        await transport.send_json({"cmd": "ota_status", "nonce": "abc"})
        if fake.notifications is None:
            fake.notifications = asyncio.Queue()
        await fake.notifications.put(
            {"cmd": "ota_status", "nonce": "abc", "phase": "running"}
        )
        result = await transport.recv_json(timeout=0.1)
        await fake.emit_permission("req-7", "approve")
        return result

    assert asyncio.run(exercise()) == {
        "cmd": "ota_status",
        "nonce": "abc",
        "phase": "running",
    }
    assert fake.writes[-1] == {"cmd": "ota_status", "nonce": "abc"}
    assert approvals == [("req-7", "approve")]


def test_native_discovery_matches_name_or_service_uuid():
    assert _matches_buddy_discovery({"name": "Codex-4DAD", "service_uuids": []}) is True
    assert _matches_buddy_discovery({"name": "Legacy-4DAD", "service_uuids": []}) is False
    assert _matches_buddy_discovery({"name": "", "service_uuids": ["6E400001-B5A3-F393-E0A9-E50E24DCCA9E"]}) is True
    assert _matches_buddy_discovery({"name": "Other", "service_uuids": ["1234"]}) is False


def test_discover_uses_native_helper_when_backend_is_native(monkeypatch):
    expected = [DiscoveredBuddy(device_id="dev-1", name="Codex-1234")]

    monkeypatch.setenv("CODEX_BUDDY_BLE_BACKEND", "native")
    monkeypatch.setattr("codex_buddy.ble_transport._discover_with_native_helper", lambda timeout: expected)

    matches = asyncio.run(BleBuddyTransport.discover(timeout=2.5))

    assert matches == expected


def test_native_helper_app_path_prefers_runtime_install(monkeypatch, tmp_path):
    app_path = tmp_path / "CodeBuddyBLEHelper.app"
    executable = app_path / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
    executable.parent.mkdir(parents=True)
    executable.write_text("#!/bin/sh\n", encoding="utf-8")

    ble_transport._native_helper_app_path.cache_clear()
    monkeypatch.delenv("CODEX_BUDDY_BLE_HELPER_APP", raising=False)
    monkeypatch.setattr(ble_transport, "runtime_helper_app_path", lambda: Path(app_path))

    try:
        assert ble_transport._native_helper_app_path() == app_path
    finally:
        ble_transport._native_helper_app_path.cache_clear()


def test_non_native_discover_requires_bleak(monkeypatch):
    monkeypatch.setattr(
        ble_transport,
        "_require_bleak",
        lambda: (_ for _ in ()).throw(RuntimeError("bleak is required")),
    )
    monkeypatch.setenv("CODEX_BUDDY_BLE_BACKEND", "bleak")

    with pytest.raises(RuntimeError, match="bleak is required"):
        asyncio.run(BleBuddyTransport.discover(timeout=0.1))


def test_non_native_connect_requires_bleak(monkeypatch):
    monkeypatch.setattr(
        ble_transport,
        "_require_bleak",
        lambda: (_ for _ in ()).throw(RuntimeError("bleak is required")),
    )

    transport = BleBuddyTransport("device-1", use_native_helper=False)

    with pytest.raises(RuntimeError, match="bleak is required"):
        asyncio.run(transport.connect())


def test_terminate_native_helper_processes_filters_by_session_dir(monkeypatch, tmp_path):
    session_dir = tmp_path / "codebuddy-ble-123"
    session_dir.mkdir()

    matching = _HelperProcess(pid=101, command=f"/tmp/CodeBuddyBLEHelper --session-dir {session_dir} --device-id dev-1")
    other = _HelperProcess(pid=202, command="/tmp/CodeBuddyBLEHelper --session-dir /tmp/other --device-id dev-1")

    snapshots = [
        [matching, other],
        [other],
    ]
    monkeypatch.setattr(
        ble_transport,
        "_list_native_helper_processes",
        lambda: snapshots.pop(0) if snapshots else [other],
    )
    monkeypatch.setattr(ble_transport.time, "monotonic", lambda: 0.0)
    monkeypatch.setattr(ble_transport.time, "sleep", lambda _: None)

    calls: list[list[str]] = []

    def fake_run(cmd: list[str], **_: object):
        calls.append(cmd)

        class _Completed:
            returncode = 0
            stdout = ""
            stderr = ""

        return _Completed()

    monkeypatch.setattr(ble_transport.subprocess, "run", fake_run)

    _terminate_native_helper_processes(session_dir=session_dir)

    assert calls == [["kill", "-TERM", "101"]]


def test_native_helper_session_cleanup_terminates_current_session_dir(monkeypatch, tmp_path):
    calls: list[dict[str, object]] = []
    monkeypatch.setattr(
        ble_transport,
        "_terminate_native_helper_processes",
        lambda **kwargs: calls.append(kwargs),
    )

    session = NativeBleHelperSession(device_id="dev-1", device_name="Codex-1234", on_permission=None)
    session._session_dir = tmp_path / "codebuddy-ble-123"
    session._session_dir.mkdir()
    session_dir = session._session_dir

    session._cleanup()

    assert calls == [{"session_dir": session_dir}]
    assert session._session_dir is None


def test_native_helper_session_start_helper_cleans_stale_helpers_for_device(monkeypatch):
    calls: list[dict[str, object]] = []

    def fake_terminate(**kwargs):
        calls.append(kwargs)

    async def fake_to_thread(fn, *args, **kwargs):
        return fn(*args, **kwargs)

    monkeypatch.setattr(ble_transport, "_terminate_native_helper_processes", fake_terminate)
    monkeypatch.setattr(ble_transport.asyncio, "to_thread", fake_to_thread)

    session = NativeBleHelperSession(device_id="dev-1", device_name="Codex-1234", on_permission=None)
    monkeypatch.setattr(session, "_launch_helper_process", lambda: None)

    async def run() -> None:
        await session._start_helper()
        assert session._pump_task is not None
        pump_task = session._pump_task
        session._cleanup()
        with pytest.raises(asyncio.CancelledError):
            await pump_task

    asyncio.run(run())

    assert calls[0] == {"device_id": "dev-1"}


def test_native_helper_connect_is_singleflight_on_same_event_loop(monkeypatch):
    async def exercise():
        session = NativeBleHelperSession(
            device_id="dev-1", device_name="Codex-1234", on_permission=None
        )
        starts = 0

        async def fake_start_helper():
            nonlocal starts
            starts += 1
            await asyncio.sleep(0)
            session._connected = True

        monkeypatch.setattr(session, "_start_helper", fake_start_helper)
        await asyncio.gather(session.connect(), session.connect())
        return starts

    assert asyncio.run(exercise()) == 1


def test_transport_connect_and_disconnect_are_singleflight_for_agent_and_ota():
    class SlowNative(_FakeNativeSession):
        def __init__(self):
            super().__init__()
            self.connects = 0
            self.disconnects = 0

        async def connect(self):
            self.connects += 1
            await asyncio.sleep(0)
            self.connected = True

        async def disconnect(self):
            self.disconnects += 1
            await asyncio.sleep(0)
            self.connected = False

    fake = SlowNative()
    factory_calls = 0

    def factory(**_):
        nonlocal factory_calls
        factory_calls += 1
        return fake

    async def exercise():
        transport = BleBuddyTransport(
            "dev-1", use_native_helper=True, native_session_factory=factory
        )
        await asyncio.gather(transport.connect(), transport.connect())
        await asyncio.gather(transport.disconnect(), transport.disconnect())

    asyncio.run(exercise())
    assert factory_calls == 1
    assert fake.connects == 1
    assert fake.disconnects == 1
    assert len(fake.writes) == 2
