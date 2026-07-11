import asyncio
from pathlib import Path

from codex_buddy.agent import BuddyAgent, ManagedSessionRuntime
from codex_buddy.catalog import SessionPrompt
from codex_buddy.events import ApprovalRequest, TurnState
from codex_buddy.proxy import ApprovalRequestResolved
from codex_buddy.state_store import BridgeStateStore, PersistedState
from codex_buddy.usage_limits import UsageDisplay


class _FakeBridge:
    def __init__(self, *, workdir, on_event, on_close, codex_path="codex") -> None:
        self.workdir = workdir
        self.on_event = on_event
        self.on_close = on_close
        self.codex_path = codex_path
        self.started = False
        self.stopped = False
        self.approvals = []
        self.proxy_url = "ws://127.0.0.1:4567"

    async def start(self) -> None:
        self.started = True

    async def stop(self) -> None:
        self.stopped = True

    async def respond_to_device_approval(self, request_id: str, decision: str) -> None:
        self.approvals.append((request_id, decision))


class _CapturingBle:
    def __init__(self, device_id="device-1") -> None:
        self.device_id = device_id
        self.sent_payloads = []
        self.disconnected = False

    async def send_snapshot(self, snapshot) -> None:
        self.sent_payloads.append(snapshot.as_ble_payload())

    async def disconnect(self) -> None:
        self.disconnected = True


class _FakeAccountUsageMonitor:
    def __init__(self, *, codex_path, codex_launch_path, on_usage) -> None:
        self.codex_path = codex_path
        self.codex_launch_path = codex_launch_path
        self.on_usage = on_usage
        self.started = asyncio.Event()
        self.stopped = False

    async def start(self) -> None:
        self.started.set()

    async def stop(self) -> None:
        self.stopped = True

    async def publish(self, usage) -> None:
        await self.on_usage(usage)


def test_agent_launch_registers_managed_session_and_routes_device_approval(tmp_path):
    created = []

    def factory(*, workdir, on_event, on_close, codex_path, codex_launch_path):
        bridge = _FakeBridge(
            workdir=workdir,
            on_event=on_event,
            on_close=on_close,
            codex_path=codex_path,
        )
        created.append(bridge)
        return bridge

    async def exercise():
        agent = BuddyAgent(
            tmp_path / "state.json",
            watcher=None,
            managed_session_factory=factory,
            clock=lambda: 120.0,
        )
        response = await agent.launch(Path("/tmp/demo"))
        bridge = created[0]
        await bridge.on_event(TurnState(thread_id="thr-1", turn_id="turn-1", active=True))
        await bridge.on_event(
            ApprovalRequest(
                thread_id="thr-1",
                turn_id="turn-1",
                request_id="req-1",
                command="rm -rf /tmp/demo",
                cwd="/tmp/demo",
                reason="Needs approval",
            )
        )
        await agent._handle_device_permission("req-1", "deny")
        return agent.status_payload(), response, bridge

    status, response, bridge = asyncio.run(exercise())

    assert response == {"ok": True, "proxy_url": "ws://127.0.0.1:4567"}
    assert bridge.started is True
    assert bridge.approvals == [("req-1", "deny")]
    assert status["snapshot"]["waiting"] == 1
    assert status["snapshot"]["prompt"]["id"] == "req-1"
    assert status["sessions"][0]["control_capability"] == "managed"
    assert status["sessions"][0]["session_id"] == "thr-1"


def test_agent_launch_passes_saved_real_codex_path_to_managed_bridge(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
            real_codex_path="/usr/local/bin/codex",
            codex_launch_path="/usr/local/bin:/usr/bin:/bin",
        )
    )
    created = []

    def factory(*, workdir, on_event, on_close, codex_path, codex_launch_path):
        bridge = _FakeBridge(
            workdir=workdir,
            on_event=on_event,
            on_close=on_close,
            codex_path=codex_path,
        )
        bridge.codex_launch_path = codex_launch_path
        created.append(bridge)
        return bridge

    async def exercise():
        agent = BuddyAgent(
            state_path,
            watcher=None,
            managed_session_factory=factory,
            clock=lambda: 120.0,
        )
        await agent.launch(Path("/tmp/demo"))

    asyncio.run(exercise())

    assert created[0].codex_path == "/usr/local/bin/codex"
    assert created[0].codex_launch_path == "/usr/local/bin:/usr/bin:/bin"


def test_managed_runtime_ignores_unrelated_approval_resolution():
    runtime = ManagedSessionRuntime(control_id="managed-1", workdir=Path("/tmp/demo"))

    runtime.apply(TurnState(thread_id="thr-1", turn_id="turn-1", active=True), now=100.0)
    runtime.apply(
        ApprovalRequest(
            thread_id="thr-1",
            turn_id="turn-1",
            request_id="req-1",
            command="rm -f /tmp/demo",
            cwd="/tmp/demo",
            reason="Needs approval",
        ),
        now=101.0,
    )

    runtime.apply(ApprovalRequestResolved(request_id="req-2"), now=102.0)

    assert runtime.state == "waiting"
    assert runtime.pending_prompt == SessionPrompt(
        request_id="req-1",
        tool="Bash",
        hint="rm -f /tmp/demo",
    )


class _FlakyBle:
    created: list["_FlakyBle"] = []

    def __init__(self, device_id: str, *, device_name: str, on_permission) -> None:
        self.device_id = device_id
        self.device_name = device_name
        self.on_permission = on_permission
        self.disconnect_calls = 0
        self.snapshot_calls = 0
        self._should_fail = not self.created
        self.created.append(self)

    async def connect(self) -> None:
        if self._should_fail:
            raise RuntimeError("temporary connect failure")

    async def disconnect(self) -> None:
        self.disconnect_calls += 1

    async def send_snapshot(self, snapshot) -> None:
        self.snapshot_calls += 1


def test_agent_ble_loop_recreates_transport_after_connect_failure(tmp_path):
    _FlakyBle.created = []
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
        )
    )

    async def exercise():
        agent = BuddyAgent(
            state_path,
            watcher=None,
            reconnect_interval=0.01,
            ble_factory=_FlakyBle,
        )
        task = asyncio.create_task(agent._ble_loop())
        await asyncio.sleep(0.08)
        agent._stopped.set()
        await task
        return agent

    agent = asyncio.run(exercise())

    assert len(_FlakyBle.created) >= 2
    assert _FlakyBle.created[0].disconnect_calls == 1
    assert agent._ble_connected is True
    assert agent._ble is _FlakyBle.created[-1]


def test_agent_publishes_account_usage_from_the_configured_monitor(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
            real_codex_path="/usr/local/bin/codex",
            codex_launch_path="/usr/local/bin:/usr/bin:/bin",
        )
    )
    created = []

    def monitor_factory(*, codex_path, codex_launch_path, on_usage):
        monitor = _FakeAccountUsageMonitor(
            codex_path=codex_path,
            codex_launch_path=codex_launch_path,
            on_usage=on_usage,
        )
        created.append(monitor)
        return monitor

    async def exercise():
        agent = BuddyAgent(
            state_path,
            socket_path=Path(f"/tmp/code-buddy-{id(state_path)}.sock"),
            watcher=None,
            keepalive_interval=60.0,
            reconnect_interval=60.0,
            account_usage_monitor_factory=monitor_factory,
        )
        ble = _CapturingBle()
        agent._ble = ble
        agent._ble_connected = True
        task = asyncio.create_task(agent.run())
        try:
            for _ in range(100):
                if created:
                    break
                await asyncio.sleep(0.01)
            assert created
            await asyncio.wait_for(created[0].started.wait(), timeout=1.0)
            await created[0].publish(UsageDisplay(five_hour_remaining=72, seven_day_remaining=91))
            status = agent.status_payload()
            persisted = BridgeStateStore(state_path).load()
        finally:
            agent._stopped.set()
            await task
        return created[0], ble, status, persisted

    monitor, ble, status, persisted = asyncio.run(exercise())

    assert monitor.codex_path == "/usr/local/bin/codex"
    assert monitor.codex_launch_path == "/usr/local/bin:/usr/bin:/bin"
    assert monitor.stopped is True
    assert ble.sent_payloads[-1]["usage"] == {
        "five_hour_remaining": 72,
        "seven_day_remaining": 91,
    }
    assert status["snapshot"]["usage"] == {
        "five_hour_remaining": 72,
        "seven_day_remaining": 91,
    }
    assert persisted.snapshot["usage"] == {
        "five_hour_remaining": 72,
        "seven_day_remaining": 91,
    }
