import asyncio

from codex_buddy import bridge
from codex_buddy.events import TokenUsage
from codex_buddy.proxy import CodexEventSource
from codex_buddy.state_store import PersistedState
from codex_buddy.token_heartbeat import TokenHeartbeat


class _ChangingStateStore:
    def __init__(self) -> None:
        self.load_calls = 0
        self.saved = None

    def load(self):
        self.load_calls += 1
        if self.load_calls == 1:
            return PersistedState(tokens_date="2026-04-20", completion_seq=41)
        return PersistedState(tokens_date="2026-04-21", completion_seq=42)

    def save(self, state) -> None:
        self.saved = state


def test_managed_token_usage_uses_official_total_token_schema_and_legacy_fallback():
    async def exercise():
        events = []

        async def on_event(event):
            events.append(event)

        source = CodexEventSource(
            upstream_url="ws://example.test",
            listen_host="127.0.0.1",
            listen_port=0,
            on_event=on_event,
        )
        await source._emit_events(
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "threadId": "thread-official",
                    "tokenUsage": {
                        "total": {
                            "inputTokens": 700,
                            "outputTokens": 200,
                            "totalTokens": 1_000,
                        }
                    },
                },
            }
        )
        await source._emit_events(
            {
                "method": "thread/tokenUsage/updated",
                "params": {"threadId": "thread-official", "tokenUsage": {"total": {}}},
            }
        )
        await source._emit_events(
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "threadId": "thread-official",
                    "tokenUsage": {"total": {"totalTokens": 1_100}},
                },
            }
        )
        await source._emit_events(
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "threadId": "thread-summed",
                    "tokenUsage": {"total": {"inputTokens": 300, "outputTokens": 125}},
                },
            }
        )
        await source._emit_events(
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "threadId": "thread-legacy",
                    "usage": {"outputTokens": 90, "sessionOutputTokens": 80},
                },
            }
        )
        heartbeat = TokenHeartbeat()
        for event in events:
            if isinstance(event, TokenUsage) and event.thread_id == "thread-official":
                heartbeat.observe(event.thread_id, event.total_tokens, now=10.0)
        return events, heartbeat._raw_values(10.0)[-1]

    events, newest_raw_sample = asyncio.run(exercise())
    assert events == [
        TokenUsage(thread_id="thread-official", total_tokens=1_000, tokens_today=1_000),
        TokenUsage(thread_id="thread-official", total_tokens=1_100, tokens_today=1_100),
        TokenUsage(thread_id="thread-summed", total_tokens=425, tokens_today=425),
        TokenUsage(thread_id="thread-legacy", total_tokens=90, tokens_today=80),
    ]
    assert newest_raw_sample == 20  # 20% leading sample of the real +100 delta.


def test_shared_app_server_launcher_uses_real_binary_and_carefully_merged_environment(monkeypatch):
    popen_calls = []

    def fake_popen(command, **kwargs):
        popen_calls.append((command, kwargs))
        return object()

    monkeypatch.setattr(bridge.subprocess, "Popen", fake_popen)
    monkeypatch.setenv("PATH", "/usr/bin:/bin")

    bridge.start_loopback_app_server(
        codex_path="/usr/local/bin/codex-real",
        codex_launch_path="/custom/node/bin:/usr/bin",
        port=43123,
    )

    command, kwargs = popen_calls[0]
    assert command == ["/usr/local/bin/codex-real", "app-server", "--listen", "ws://127.0.0.1:43123"]
    assert kwargs["env"]["CODE_BUDDY_SHIM_ACTIVE"] == "1"
    assert kwargs["env"]["PATH"].split(bridge.os.pathsep)[:3] == [
        "/usr/local/bin",
        "/custom/node/bin",
        "/usr/bin",
    ]
    assert kwargs["start_new_session"] is True


def test_managed_session_bridge_starts_app_server_with_real_codex_path(monkeypatch):
    commands = []

    class FakeProcess:
        def terminate(self) -> None:
            pass

        def wait(self, timeout=None) -> int:
            return 0

    def fake_popen(command, **kwargs):
        commands.append(command)
        return FakeProcess()

    class FakeResponse:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

    def fake_urlopen(url, timeout):
        return FakeResponse()

    monkeypatch.setattr(bridge.subprocess, "Popen", fake_popen)
    monkeypatch.setattr(bridge.urllib.request, "urlopen", fake_urlopen)

    async def on_event(event):
        return None

    async def exercise():
        session = bridge.ManagedSessionBridge(
            workdir=bridge.Path("/tmp/demo"),
            codex_path="/usr/local/bin/codex",
            on_event=on_event,
        )
        await session._start_upstream()

    asyncio.run(exercise())

    assert commands[0][:2] == ["/usr/local/bin/codex", "app-server"]


def test_managed_session_bridge_uses_saved_launch_path_for_codex_process(monkeypatch):
    popen_calls = []

    class FakeProcess:
        def terminate(self) -> None:
            pass

        def wait(self, timeout=None) -> int:
            return 0

    def fake_popen(command, **kwargs):
        popen_calls.append((command, kwargs))
        return FakeProcess()

    class FakeResponse:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return False

    def fake_urlopen(url, timeout):
        return FakeResponse()

    monkeypatch.setattr(bridge.subprocess, "Popen", fake_popen)
    monkeypatch.setattr(bridge.urllib.request, "urlopen", fake_urlopen)
    monkeypatch.setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin")

    async def on_event(event):
        return None

    async def exercise():
        session = bridge.ManagedSessionBridge(
            workdir=bridge.Path("/tmp/demo"),
            codex_path="/usr/local/bin/codex",
            codex_launch_path="/custom/node/bin:/usr/bin:/bin",
            on_event=on_event,
        )
        await session._start_upstream()

    asyncio.run(exercise())

    _, kwargs = popen_calls[0]
    env_path = kwargs["env"]["PATH"].split(bridge.os.pathsep)
    assert "/custom/node/bin" in env_path
    assert "/usr/local/bin" in env_path
    assert env_path.index("/usr/local/bin") < env_path.index("/usr/bin")
    assert kwargs["env"]["CODE_BUDDY_SHIM_ACTIVE"] == "1"
    assert kwargs["start_new_session"] is True


def test_legacy_bridge_persists_one_consistent_state_read(tmp_path):
    controller = bridge.BridgeController(
        bridge.RunConfig(
            workdir=tmp_path,
            prompt=None,
            state_path=tmp_path / "state.json",
            paired_device_id="device-1",
            paired_device_name="Codex-4DAD",
        )
    )
    store = _ChangingStateStore()
    controller.store = store

    controller._persist_snapshot(controller.reducer.snapshot(), buddy_connected=False)

    assert store.load_calls == 1
    assert store.saved.tokens_date == "2026-04-20"
    assert store.saved.completion_seq == 41
