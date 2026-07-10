import asyncio

from codex_buddy import bridge


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
