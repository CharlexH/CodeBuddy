import asyncio
import base64
import hashlib
from dataclasses import replace
from pathlib import Path

import pytest

from codex_buddy.agent import BuddyAgent, ManagedSessionRuntime
from codex_buddy.bridge import BridgeController, RunConfig
from codex_buddy.catalog import SessionPrompt, SessionRecord
from codex_buddy.events import AgentOutput, ApprovalRequest, TurnState
from codex_buddy.proxy import ApprovalRequestResolved
from codex_buddy.state_store import BridgeStateStore, PersistedState
from codex_buddy.usage_limits import UsageDisplay
from codex_buddy.ota_release import OtaImageInfo


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


class _BlockingBle:
    def __init__(self) -> None:
        self.started = asyncio.Event()
        self.release = asyncio.Event()

    async def send_snapshot(self, snapshot) -> None:
        self.started.set()
        await self.release.wait()

    async def disconnect(self) -> None:
        pass


class _FakeAccountUsageMonitor:
    def __init__(self, *, codex_path, codex_launch_path, on_usage, on_thread_ids=None) -> None:
        self.codex_path = codex_path
        self.codex_launch_path = codex_launch_path
        self.on_usage = on_usage
        self.on_thread_ids = on_thread_ids
        self.started = asyncio.Event()
        self.stopped = False

    async def start(self) -> None:
        self.started.set()

    async def stop(self) -> None:
        self.stopped = True

    async def publish(self, usage) -> None:
        await self.on_usage(usage)

    async def publish_thread_ids(self, thread_ids) -> None:
        if self.on_thread_ids is not None:
            await self.on_thread_ids(frozenset(thread_ids))


class _FakeClientStateWatcher:
    def __init__(self, values) -> None:
        self.values = iter(values)
        self.visible_thread_ids = []

    def poll(self, visible_thread_ids=None):
        self.visible_thread_ids.append(visible_thread_ids)
        return next(self.values)


def test_agent_refreshes_optional_unread_count_from_client_state(tmp_path):
    client_state_watcher = _FakeClientStateWatcher([None, 4, 2])
    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=None,
        client_state_watcher=client_state_watcher,
    )

    agent._refresh_readonly_state()
    assert agent._snapshot().unread is None
    assert "unread" not in agent._snapshot().as_ble_payload()

    agent._refresh_readonly_state()
    assert agent._snapshot().unread == 4
    assert agent._snapshot().as_ble_payload()["unread"] == 4

    agent._refresh_readonly_state()
    assert agent._snapshot().unread == 2


def test_agent_recomputes_unread_when_visible_thread_ids_arrive(tmp_path):
    client_state_watcher = _FakeClientStateWatcher([0])
    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=None,
        client_state_watcher=client_state_watcher,
    )

    asyncio.run(agent._handle_visible_thread_ids(frozenset({"thread-visible"})))

    assert client_state_watcher.visible_thread_ids == [frozenset({"thread-visible"})]
    assert agent._snapshot().unread == 0


def test_agent_initial_snapshot_explicitly_clears_a_meter_left_by_a_prior_agent(tmp_path):
    agent = BuddyAgent(
        tmp_path / "state.json",
        socket_path=tmp_path / "agent.sock",
        watcher=None,
    )

    assert agent._snapshot().as_ble_payload()["usage"] is None


def test_agent_loads_persisted_completion_sequence_into_snapshot(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(PersistedState(completion_seq=41))

    agent = BuddyAgent(state_path, watcher=None)

    assert agent._snapshot().as_ble_payload()["completion_seq"] == 41


def test_managed_events_populate_twenty_second_activity_heartbeat(tmp_path):
    now = [100.0]
    agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: now[0])
    agent._managed_runtime["managed-1"] = ManagedSessionRuntime(
        control_id="managed-1",
        workdir=tmp_path,
    )

    asyncio.run(
        agent._handle_managed_event(
            "managed-1",
            TurnState(thread_id="thr-1", turn_id="turn-1", active=True),
        )
    )
    assert agent._snapshot().as_ble_payload()["activity20"] == 0b1

    now[0] = 101.0
    assert agent._snapshot().as_ble_payload()["activity20"] == 0b10


def test_agent_projects_per_session_token_deltas_into_versioned_heartbeat(tmp_path):
    now = [100.0]
    agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: now[0])
    record = SessionRecord(
        session_id="readonly-1",
        source="desktop",
        originator="codex",
        cwd=str(tmp_path),
        state="running",
        last_activity_at=now[0],
        latest_message="Working",
        entries=[],
        tokens_total=1_000,
        tokens_session=1_000,
        control_capability="readonly",
    )
    agent.catalog.upsert(record)

    initial = _decode_token_heartbeat(agent._snapshot().as_ble_payload()["token20v1"])
    assert initial == bytes(64)

    agent.catalog.upsert(replace(record, tokens_total=1_500))
    increased = _decode_token_heartbeat(agent._snapshot().as_ble_payload()["token20v1"])
    assert increased[-1] > 0

    agent.catalog.upsert(replace(record, tokens_total=100))
    rebaselined = _decode_token_heartbeat(agent._snapshot().as_ble_payload()["token20v1"])
    assert rebaselined == increased


def test_agent_combines_managed_and_readonly_token_deltas_without_aggregate_spikes(tmp_path):
    now = [100.0]
    agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: now[0])
    readonly = SessionRecord(
        session_id="readonly-1",
        source="desktop",
        originator="codex",
        cwd=str(tmp_path),
        state="running",
        last_activity_at=now[0],
        latest_message="Working",
        entries=[],
        tokens_total=1_000,
        tokens_session=1_000,
        control_capability="readonly",
    )
    managed = replace(
        readonly,
        session_id="managed-1",
        source="managed",
        originator="code-buddy",
        tokens_total=2_000,
        tokens_session=2_000,
        control_capability="managed",
    )
    agent.catalog.upsert(readonly)
    agent.catalog.upsert(managed)
    assert _decode_token_heartbeat(agent._snapshot().token20v1) == bytes(64)

    agent.catalog.upsert(replace(readonly, tokens_total=1_100))
    agent.catalog.upsert(replace(managed, tokens_total=2_200))
    samples = _decode_token_heartbeat(agent._snapshot().token20v1)

    assert samples[-1] > 0


def test_agent_prunes_absent_token_baselines_before_a_session_reappears(tmp_path):
    now = [100.0]
    agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: now[0])
    record = SessionRecord(
        session_id="readonly-1",
        source="desktop",
        originator="codex",
        cwd=str(tmp_path),
        state="running",
        last_activity_at=now[0],
        latest_message="Working",
        entries=[],
        tokens_total=1_000,
        tokens_session=1_000,
        control_capability="readonly",
    )
    agent.catalog.upsert(record)
    assert _decode_token_heartbeat(agent._snapshot().token20v1) == bytes(64)

    agent.catalog.remove(record.session_id)
    agent._snapshot()
    agent.catalog.upsert(replace(record, tokens_total=5_000))

    assert _decode_token_heartbeat(agent._snapshot().token20v1) == bytes(64)


def _decode_token_heartbeat(encoded: str) -> bytes:
    return base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4))


def test_agent_persists_current_completion_sequence(tmp_path):
    state_path = tmp_path / "state.json"
    agent = BuddyAgent(state_path, watcher=None)
    agent._completion_seq = 42

    snapshot = agent._snapshot()
    agent._persist(snapshot, agent_running=True)

    persisted = BridgeStateStore(state_path).load()
    assert persisted.completion_seq == 42
    assert persisted.snapshot["completion_seq"] == 42


def test_agent_persists_completed_sequence_before_ble_publish_finishes(tmp_path):
    async def exercise():
        state_path = tmp_path / "state.json"
        agent = BuddyAgent(state_path, watcher=None, clock=lambda: 100.0)
        ble = _BlockingBle()
        agent._ble = ble
        agent._ble_connected = True
        agent._managed_runtime["managed-1"] = ManagedSessionRuntime(
            control_id="managed-1",
            workdir=tmp_path,
        )

        publish = asyncio.create_task(
            agent._handle_managed_event(
                "managed-1",
                TurnState(
                    thread_id="thr-1",
                    turn_id="turn-1",
                    active=False,
                    status="completed",
                ),
            )
        )
        await asyncio.wait_for(ble.started.wait(), timeout=1.0)
        try:
            persisted = BridgeStateStore(state_path).load()
            restarted = BuddyAgent(state_path, watcher=None)
            assert publish.done() is False
            assert persisted.completion_seq == 1
            assert restarted._snapshot().as_ble_payload()["completion_seq"] == 1
        finally:
            ble.release.set()
            await publish

    asyncio.run(exercise())


def test_legacy_bridge_preserves_current_completion_sequence(tmp_path):
    state_path = tmp_path / "state.json"
    store = BridgeStateStore(state_path)
    store.save(PersistedState(completion_seq=41))
    controller = BridgeController(
        RunConfig(
            workdir=tmp_path,
            prompt=None,
            state_path=state_path,
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
        )
    )

    controller._persist_snapshot(controller.reducer.snapshot(), buddy_connected=False)

    assert store.load().completion_seq == 41


def test_agent_publishes_one_completion_sequence_per_successful_turn(tmp_path):
    async def exercise():
        agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: 100.0)
        ble = _CapturingBle()
        agent._ble = ble
        agent._ble_connected = True
        agent._managed_runtime["managed-1"] = ManagedSessionRuntime(
            control_id="managed-1",
            workdir=tmp_path,
        )

        await agent._handle_managed_event(
            "managed-1",
            TurnState(thread_id="thr-1", turn_id="turn-1", active=True),
        )
        await agent._handle_managed_event(
            "managed-1",
            AgentOutput(thread_id="thr-1", text="item completed"),
        )
        assert agent._snapshot().as_ble_payload()["completion_seq"] == 0

        completed = TurnState(
            thread_id="thr-1",
            turn_id="turn-1",
            active=False,
            status="completed",
        )
        await agent._handle_managed_event("managed-1", completed)
        await agent._handle_managed_event("managed-1", completed)

        await agent._handle_managed_event(
            "managed-1",
            TurnState(
                thread_id="thr-1",
                turn_id="turn-failed",
                active=False,
                status="failed",
            ),
        )
        await agent._handle_managed_event(
            "managed-1",
            TurnState(
                thread_id="thr-1",
                turn_id="turn-interrupted",
                active=False,
                status="interrupted",
            ),
        )
        return agent, ble

    agent, ble = asyncio.run(exercise())

    assert agent._snapshot().as_ble_payload()["completion_seq"] == 1
    assert [payload["completion_seq"] for payload in ble.sent_payloads].count(1) == 1


def test_agent_publishes_one_completion_sequence_for_a_desktop_turn(tmp_path):
    def readonly(state, completed_turn_id):
        return SessionRecord(
            session_id="desktop-1",
            source="vscode",
            originator="Codex Desktop",
            cwd=str(tmp_path),
            state=state,
            last_activity_at=100.0,
            latest_message="Done",
            entries=["Done"],
            tokens_total=0,
            tokens_session=0,
            control_capability="readonly",
            completed_turn_id=completed_turn_id,
        )

    class _ReadonlyWatcher:
        def __init__(self):
            self.values = iter(
                [
                    [readonly("completed", "turn-old")],
                    [readonly("running", "turn-old")],
                    [readonly("completed", "turn-new")],
                    [readonly("completed", "turn-new")],
                ]
            )

        def poll(self, now):
            return next(self.values)

    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=_ReadonlyWatcher(),
        client_state_watcher=_FakeClientStateWatcher([None, None, None, None]),
        clock=lambda: 100.0,
    )

    agent._refresh_readonly_state()
    assert agent._snapshot().as_ble_payload()["completion_seq"] == 0
    agent._refresh_readonly_state()
    assert agent._snapshot().as_ble_payload()["completion_seq"] == 0
    agent._refresh_readonly_state()
    assert agent._snapshot().as_ble_payload()["completion_seq"] == 1
    agent._refresh_readonly_state()
    assert agent._snapshot().as_ble_payload()["completion_seq"] == 1


def test_agent_chimes_for_a_new_desktop_session_but_not_its_subagents(tmp_path):
    def readonly(session_id, source, turn_id):
        return SessionRecord(
            session_id=session_id,
            source=source,
            originator="Codex Desktop",
            cwd=str(tmp_path),
            state="completed",
            last_activity_at=100.0,
            latest_message="Done",
            entries=["Done"],
            tokens_total=0,
            tokens_session=0,
            control_capability="readonly",
            completed_turn_id=turn_id,
        )

    class _ReadonlyWatcher:
        def __init__(self):
            self.values = iter(
                [
                    [],
                    [
                        readonly("desktop-1", "vscode", "turn-main"),
                        readonly("desktop-child", "subagent", "turn-child"),
                    ],
                ]
            )

        def poll(self, now):
            return next(self.values)

    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=_ReadonlyWatcher(),
        client_state_watcher=_FakeClientStateWatcher([None, None]),
        clock=lambda: 100.0,
    )

    agent._refresh_readonly_state()
    agent._refresh_readonly_state()

    assert agent._snapshot().as_ble_payload()["completion_seq"] == 1


def test_agent_completion_sequence_wraps_and_deduplication_is_bounded(tmp_path):
    async def exercise():
        agent = BuddyAgent(tmp_path / "state.json", watcher=None, clock=lambda: 100.0)
        agent._managed_runtime["managed-1"] = ManagedSessionRuntime(
            control_id="managed-1",
            workdir=tmp_path,
        )
        agent._completion_seq = 0xFFFFFFFF

        for index in range(300):
            await agent._handle_managed_event(
                "managed-1",
                TurnState(
                    thread_id="thr-1",
                    turn_id=f"turn-{index}",
                    active=False,
                    status="completed",
                ),
            )
        return agent

    agent = asyncio.run(exercise())

    assert agent._snapshot().as_ble_payload()["completion_seq"] == 299
    assert len(agent._completed_turn_order) <= 256
    assert len(agent._completed_turn_keys) <= 256


def test_agent_constructs_after_closed_loop_and_runs_in_fresh_loop(tmp_path):
    asyncio.run(asyncio.sleep(0))
    socket_path = Path(f"/tmp/code-buddy-loop-{id(tmp_path)}.sock")
    agent = BuddyAgent(
        tmp_path / "state.json",
        socket_path=socket_path,
        watcher=None,
        readonly_poll_interval=60.0,
        keepalive_interval=60.0,
        reconnect_interval=60.0,
    )

    async def exercise():
        task = asyncio.create_task(agent.run())
        for _ in range(100):
            if agent._server is not None:
                break
            await asyncio.sleep(0.001)
        assert agent._server is not None
        assert agent.socket_path.stat().st_mode & 0o777 == 0o600
        assert await agent._handle_command({"cmd": "stop"}) == {"ok": True}
        await task

    asyncio.run(exercise())

    assert not agent.socket_path.exists()


def test_agent_rejects_wrong_uid_before_reading_a_command(tmp_path, monkeypatch):
    class _Reader:
        async def readline(self):
            raise AssertionError("command must not be read from an unauthorized peer")

    class _Writer:
        def __init__(self):
            self.output = bytearray()
            self.closed = False

        def get_extra_info(self, name):
            assert name == "socket"
            return object()

        def write(self, data):
            self.output.extend(data)

        async def drain(self):
            pass

        def close(self):
            self.closed = True

        async def wait_closed(self):
            pass

    def reject(_peer_socket):
        raise PermissionError("local agent client is not authorized")

    monkeypatch.setattr("codex_buddy.agent.require_current_user_peer", reject)
    writer = _Writer()
    agent = BuddyAgent(tmp_path / "state.json", watcher=None)

    asyncio.run(agent._handle_client(_Reader(), writer))

    assert writer.closed is True
    assert b'"ok":false' in writer.output
    assert b"credential" not in writer.output


def test_agent_ota_session_lock_is_recreated_for_sequential_event_loops(tmp_path):
    agent = BuddyAgent(tmp_path / "state.json", watcher=None)

    async def exercise_contention():
        entered = asyncio.Event()
        release = asyncio.Event()
        visits = []

        async def visit(name):
            async with agent.ota_session():
                visits.append(name)
                if name == "first":
                    entered.set()
                    await release.wait()

        first = asyncio.create_task(visit("first"))
        await entered.wait()
        second = asyncio.create_task(visit("second"))
        await asyncio.sleep(0)
        assert visits == ["first"]
        release.set()
        await asyncio.gather(first, second)
        assert visits == ["first", "second"]

    asyncio.run(exercise_contention())
    asyncio.run(exercise_contention())


def test_agent_ota_session_suspends_snapshots_and_releases_after_failure(tmp_path):
    async def exercise():
        agent = BuddyAgent(tmp_path / "state.json", watcher=None)
        ble = _CapturingBle()
        agent._ble = ble
        agent._ble_connected = True

        await agent._publish_state(force=True)
        assert len(ble.sent_payloads) == 1

        with pytest.raises(RuntimeError, match="install failed"):
            async with agent.ota_session():
                assert agent.ota_session_active is True
                await agent._publish_state(force=True)
                assert len(ble.sent_payloads) == 1
                raise RuntimeError("install failed")

        assert agent.ota_session_active is False
        async with agent.ota_session():
            assert agent.ota_session_active is True
        await agent._publish_state(force=True)
        assert len(ble.sent_payloads) == 2

    asyncio.run(exercise())


def test_agent_ota_commands_are_exclusive_cancel_and_release_snapshot_suspension(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"firmware")

    def inspect(path):
        assert path != image
        assert path.read_bytes() == b"firmware"
        return OtaImageInfo(
            path=path,
            version="0.1.5",
            size_bytes=8,
            sha256=hashlib.sha256(b"firmware").hexdigest(),
        )

    async def exercise():
        entered = asyncio.Event()
        cancelled = asyncio.Event()
        agent = BuddyAgent(
            tmp_path / "state.json",
            watcher=None,
            ota_image_inspector=inspect,
            ota_nonce_factory=lambda: "n" * 24,
        )
        agent._ota_snapshot_root = tmp_path / "snapshots"
        agent._ble = _CapturingBle()
        agent._ble_connected = True

        async def fake_run(session, inspected):
            entered.set()
            session.phase = "await-confirm"
            await session.cancel_event.wait()
            cancelled.set()
            session.phase = "cancelled"
            session.terminal = True
            session.success = False

        agent._ota_coordinator.run = fake_run
        first = await agent._handle_command(
            {"cmd": "ota_begin", "firmware": str(image)}
        )
        await entered.wait()
        assert agent.ota_session_active is True
        with pytest.raises(RuntimeError, match="already active"):
            await agent._handle_command({"cmd": "ota_begin", "firmware": str(image)})
        status = await agent._handle_command(
            {"cmd": "ota_status", "nonce": "n" * 24}
        )
        assert status["ota"]["phase"] == "await-confirm"
        await agent._handle_command({"cmd": "ota_cancel", "nonce": "n" * 24})
        await cancelled.wait()
        await agent._ota_task
        assert agent.ota_session_active is False
        return first

    first = asyncio.run(exercise())
    assert first["ota"]["nonce"] == "n" * 24


def test_ota_begin_claims_exclusive_state_before_immediate_launch(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"original-firmware")

    async def exercise():
        release = asyncio.Event()
        agent = BuddyAgent(
            tmp_path / "state.json",
            watcher=None,
            ota_image_inspector=lambda path: OtaImageInfo(
                path=path,
                version="0.1.5",
                size_bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            ),
            ota_nonce_factory=lambda: "n" * 24,
        )
        agent._ota_snapshot_root = tmp_path / "snapshots"
        agent._ble = _CapturingBle()
        agent._ble_connected = True

        async def fake_coordinate(session, inspected):
            await release.wait()
            session.terminal = True

        agent._ota_coordinator.run = fake_coordinate
        await agent._handle_command({"cmd": "ota_begin", "firmware": str(image)})
        assert agent.ota_session_active is True
        with pytest.raises(Exception, match="firmware update is active"):
            await agent._handle_command({"cmd": "launch", "workdir": str(tmp_path)})
        release.set()
        await agent._ota_task
        assert agent.ota_session_active is False

    asyncio.run(exercise())


def test_ota_begin_uses_private_snapshot_when_original_path_is_replaced(tmp_path):
    image = tmp_path / "firmware.bin"
    original = b"original-firmware"
    image.write_bytes(original)

    async def exercise():
        continue_run = asyncio.Event()
        observed = {}
        agent = BuddyAgent(
            tmp_path / "state.json",
            watcher=None,
            ota_image_inspector=lambda path: OtaImageInfo(
                path=path,
                version="0.1.5",
                size_bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            ),
            ota_nonce_factory=lambda: "n" * 24,
        )
        agent._ota_snapshot_root = tmp_path / "snapshots"
        agent._ble = _CapturingBle()
        agent._ble_connected = True

        async def fake_coordinate(session, inspected):
            await continue_run.wait()
            observed["path"] = inspected.path
            observed["contents"] = inspected.path.read_bytes()
            session.terminal = True

        agent._ota_coordinator.run = fake_coordinate
        await agent._handle_command({"cmd": "ota_begin", "firmware": str(image)})
        image.write_bytes(b"replacement-attacker-content")
        continue_run.set()
        await agent._ota_task
        return observed

    observed = asyncio.run(exercise())
    assert observed["path"] != image
    assert observed["contents"] == original
    assert not observed["path"].exists()


def test_ota_begin_inspection_failure_cleans_snapshot_and_releases_claim(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"invalid-firmware")
    snapshots = tmp_path / "snapshots"
    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=None,
        ota_image_inspector=lambda _: (_ for _ in ()).throw(ValueError("invalid image")),
    )
    agent._ota_snapshot_root = snapshots
    agent._ble = _CapturingBle()
    agent._ble_connected = True

    with pytest.raises(ValueError, match="invalid image"):
        asyncio.run(
            agent._handle_command({"cmd": "ota_begin", "firmware": str(image)})
        )

    assert agent.ota_session_active is False
    assert snapshots.is_dir()
    assert list(snapshots.iterdir()) == []


def test_ota_claim_from_closed_event_loop_fails_closed_on_another_loop(tmp_path):
    agent = BuddyAgent(tmp_path / "state.json", watcher=None)
    asyncio.run(agent._claim_ota_session())

    with pytest.raises(RuntimeError, match="already active|another event loop"):
        asyncio.run(agent._claim_ota_session())

    assert agent.ota_session_active is True


def test_agent_rejects_ota_when_approval_is_pending(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"firmware")
    agent = BuddyAgent(
        tmp_path / "state.json",
        watcher=None,
        ota_image_inspector=lambda _: OtaImageInfo(
            path=image, version="0.1.5", size_bytes=8, sha256="a" * 64
        ),
    )
    runtime = ManagedSessionRuntime(control_id="m", workdir=tmp_path)
    runtime.session_id = "thread"
    runtime.pending_prompt = SessionPrompt("request", "Bash", "write")
    agent._managed_runtime["m"] = runtime
    agent._ble_connected = True

    with pytest.raises(RuntimeError, match="approval"):
        asyncio.run(agent._handle_command({"cmd": "ota_begin", "firmware": str(image)}))


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
        await agent._handle_command({"cmd": "stop"})
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

    def monitor_factory(*, codex_path, codex_launch_path, on_usage, on_thread_ids):
        monitor = _FakeAccountUsageMonitor(
            codex_path=codex_path,
            codex_launch_path=codex_launch_path,
            on_usage=on_usage,
            on_thread_ids=on_thread_ids,
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
            await created[0].publish(UsageDisplay(seven_day_remaining=99))
            await created[0].publish(UsageDisplay(five_hour_remaining=72, seven_day_remaining=91))
            await created[0].publish(None)
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
    assert ble.sent_payloads[-3]["usage"] == {"seven_day_remaining": 99}
    assert ble.sent_payloads[-2]["usage"] == {
        "five_hour_remaining": 72,
        "seven_day_remaining": 91,
    }
    assert ble.sent_payloads[-1]["usage"] is None
    assert status["snapshot"]["usage"] is None
    assert persisted.snapshot["usage"] is None


def test_agent_retries_account_usage_monitor_after_startup_failure(tmp_path):
    state_path = tmp_path / "state.json"
    BridgeStateStore(state_path).save(
        PersistedState(
            real_codex_path="/usr/local/bin/codex",
            codex_launch_path="/usr/local/bin:/usr/bin:/bin",
        )
    )
    created = []

    class _FailingAccountUsageMonitor(_FakeAccountUsageMonitor):
        async def start(self) -> None:
            self.started.set()
            raise RuntimeError("app-server was not ready")

    def monitor_factory(*, codex_path, codex_launch_path, on_usage, on_thread_ids):
        monitor_type = _FailingAccountUsageMonitor if not created else _FakeAccountUsageMonitor
        monitor = monitor_type(
            codex_path=codex_path,
            codex_launch_path=codex_launch_path,
            on_usage=on_usage,
            on_thread_ids=on_thread_ids,
        )
        created.append(monitor)
        return monitor

    async def exercise():
        agent = BuddyAgent(
            state_path,
            socket_path=Path(f"/tmp/code-buddy-retry-{id(state_path)}.sock"),
            watcher=None,
            keepalive_interval=60.0,
            reconnect_interval=0.01,
            account_usage_monitor_factory=monitor_factory,
        )
        task = asyncio.create_task(agent.run())
        try:
            for _ in range(100):
                if len(created) >= 2:
                    break
                await asyncio.sleep(0.01)
            assert len(created) >= 2
            await asyncio.wait_for(created[1].started.wait(), timeout=1.0)
            await created[1].publish(UsageDisplay(seven_day_remaining=89))
            return agent._snapshot().as_ble_payload()
        finally:
            agent._stopped.set()
            await task

    payload = asyncio.run(exercise())

    assert created[0].stopped is True
    assert created[1].stopped is True
    assert payload["usage"] == {"seven_day_remaining": 89}
