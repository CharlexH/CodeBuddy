import argparse
import asyncio
import io
import json
import re
from typing import Optional

import pytest

from codex_buddy import cli


def _project_version() -> str:
    text = (cli.Path(__file__).resolve().parents[1] / "pyproject.toml").read_text()
    match = re.search(r'^version = "([^"]+)"$', text, re.MULTILINE)
    assert match is not None
    return match.group(1)


def test_main_runs_setup_when_no_subcommand_and_setup_incomplete(monkeypatch):
    seen: dict[str, object] = {}

    def fake_is_setup_complete(args: argparse.Namespace) -> bool:
        seen["checked_state_path"] = args.state_path
        return False

    def fake_setup(args: argparse.Namespace) -> int:
        seen["command"] = args.command
        seen["state_path"] = args.state_path
        return 7

    monkeypatch.setattr(cli, "_is_setup_complete", fake_is_setup_complete)
    monkeypatch.setattr(cli, "_setup", fake_setup)

    exit_code = cli.main([])

    assert exit_code == 7
    assert seen == {
        "checked_state_path": cli.default_state_path(),
        "command": "default",
        "state_path": cli.default_state_path(),
    }


def test_main_shows_status_when_no_subcommand_and_setup_complete(monkeypatch):
    events = []

    def fake_is_setup_complete(args: argparse.Namespace) -> bool:
        events.append(("is_setup_complete", args.state_path))
        return True

    def fake_default_status(args: argparse.Namespace) -> int:
        events.append(("default_status", args.state_path, args.command))
        return 11

    monkeypatch.setattr(cli, "_is_setup_complete", fake_is_setup_complete)
    monkeypatch.setattr(cli, "_default_status", fake_default_status)

    exit_code = cli.main([])

    assert exit_code == 11
    assert events == [
        ("is_setup_complete", cli.default_state_path()),
        ("default_status", cli.default_state_path(), "default"),
    ]


def test_pair_resends_time_sync_before_disconnect(monkeypatch):
    events: list[object] = []

    class FakeTransport:
        def __init__(self, device_id: str, *, device_name: Optional[str] = None, **_: object) -> None:
            events.append(("init", device_id, device_name))

        @classmethod
        async def discover(cls, *, timeout: float = 4.0):
            events.append(("discover", timeout))
            return [argparse.Namespace(device_id="dev-1", name="Codex-1234")]

        async def connect(self) -> None:
            events.append("connect")

        async def send_time_sync(self) -> None:
            events.append("time_sync")

        async def disconnect(self) -> None:
            events.append("disconnect")

    class FakeStore:
        def __init__(self, path) -> None:
            events.append(("store_init", path))

        def load(self):
            return cli.PersistedState(tokens_today=3, tokens_date="2026-04-20", tokens_total=9)

        def save(self, state) -> None:
            events.append(("save", state.paired_device_id, state.paired_device_name, state.tokens_today, state.tokens_total))

    async def fake_sleep(seconds: float) -> None:
        events.append(("sleep", seconds))

    monkeypatch.setattr(cli, "BleBuddyTransport", FakeTransport)
    monkeypatch.setattr(cli, "BridgeStateStore", FakeStore)
    monkeypatch.setattr(cli.asyncio, "sleep", fake_sleep)

    args = argparse.Namespace(
        state_path="/tmp/codebuddy-state.json",
        device=None,
        timeout=4.0,
        command="pair",
    )

    exit_code = asyncio.run(cli._pair(args))

    assert exit_code == 0
    assert ("discover", 4.0) in events
    assert ("init", "dev-1", "Codex-1234") in events
    assert "connect" in events
    assert "time_sync" in events
    assert ("sleep", 0.25) in events
    assert "disconnect" in events


def test_pair_prompts_for_choice_when_multiple_devices_found(monkeypatch):
    events: list[object] = []

    class FakeTransport:
        def __init__(self, device_id: str, *, device_name: Optional[str] = None, **_: object) -> None:
            events.append(("init", device_id, device_name))

        @classmethod
        async def discover(cls, *, timeout: float = 4.0):
            return [
                argparse.Namespace(device_id="dev-1", name="Codex-1111"),
                argparse.Namespace(device_id="dev-2", name="Codex-2222"),
            ]

        async def connect(self) -> None:
            events.append("connect")

        async def send_time_sync(self) -> None:
            events.append("time_sync")

        async def disconnect(self) -> None:
            events.append("disconnect")

    class FakeStore:
        def __init__(self, path) -> None:
            pass

        def load(self):
            return cli.PersistedState()

        def save(self, state) -> None:
            events.append(("saved", state.paired_device_id, state.paired_device_name))

    async def fake_sleep(seconds: float) -> None:
        events.append(("sleep", seconds))

    monkeypatch.setattr(cli, "BleBuddyTransport", FakeTransport)
    monkeypatch.setattr(cli, "BridgeStateStore", FakeStore)
    monkeypatch.setattr(cli.asyncio, "sleep", fake_sleep)
    monkeypatch.setattr("builtins.input", lambda _: "2")

    args = argparse.Namespace(
        state_path="/tmp/codebuddy-state.json",
        device=None,
        timeout=4.0,
        command="pair",
    )

    exit_code = asyncio.run(cli._pair(args))

    assert exit_code == 0
    assert ("init", "dev-2", "Codex-2222") in events
    assert ("saved", "dev-2", "Codex-2222") in events


def test_run_uses_agent_launch_and_executes_local_codex_remote(monkeypatch):
    events: list[object] = []

    class FakeStore:
        def __init__(self, path) -> None:
            events.append(("store_init", path))

        def load(self):
            return cli.PersistedState(paired_device_id="dev-1", paired_device_name="Codex-1234")

    async def fake_ensure_agent_running(state_path) -> None:
        events.append(("ensure_agent", state_path))

    async def fake_agent_request(state_path, payload):
        events.append(("agent_request", state_path, payload))
        return {"ok": True, "proxy_url": "ws://127.0.0.1:4567"}

    class FakeProcess:
        async def wait(self) -> int:
            return 23

    async def fake_create_subprocess_exec(*command, **kwargs):
        events.append(("spawn", command, kwargs))
        return FakeProcess()

    monkeypatch.setattr(cli, "BridgeStateStore", FakeStore)
    monkeypatch.setattr(cli, "_ensure_agent_running", fake_ensure_agent_running)
    monkeypatch.setattr(cli, "_agent_request", fake_agent_request)
    monkeypatch.setattr(cli.asyncio, "create_subprocess_exec", fake_create_subprocess_exec)

    args = argparse.Namespace(
        state_path="/tmp/codebuddy-state.json",
        workdir=cli.Path("/tmp/demo"),
        prompt="Inspect this project",
        command="run",
    )

    exit_code = asyncio.run(cli._run(args))

    assert exit_code == 23
    assert ("ensure_agent", "/tmp/codebuddy-state.json") in events
    assert (
        "agent_request",
        "/tmp/codebuddy-state.json",
        {"cmd": "launch", "workdir": "/tmp/demo"},
    ) in events
    spawn = next(item for item in events if item[0] == "spawn")
    assert spawn[1] == (
        "codex",
        "--remote",
        "ws://127.0.0.1:4567",
        "-a",
        "untrusted",
        "-C",
        "/tmp/demo",
        "Inspect this project",
    )


def test_setup_records_current_path_for_codex_subprocesses(tmp_path, monkeypatch):
    state_path = tmp_path / "state.json"
    helper_path = tmp_path / "helper" / "CodeBuddyBLEHelper.app"
    helper_path.mkdir(parents=True)
    selected = argparse.Namespace(device_id="dev-1", name="Codex-1234")

    async def fake_resolve_selected_device(args, current):
        return selected

    async def fake_pair_selected_device(store, selected_device):
        current = store.load()
        store.save(
            cli.replace(
                current,
                paired_device_id=selected_device.device_id,
                paired_device_name=selected_device.name,
            )
        )

    def fake_write_codex_shim(shim_path, *, python_executable):
        return None

    monkeypatch.setattr(cli.sys, "platform", "darwin")
    monkeypatch.setenv("PATH", "/custom/node/bin:/usr/bin:/bin")
    monkeypatch.setattr(cli.setup_flow, "migrate_legacy_state", lambda: False)
    monkeypatch.setattr(cli.setup_flow, "ensure_helper_app_installed", lambda: helper_path)
    monkeypatch.setattr(
        cli.setup_flow,
        "resolve_real_codex_path",
        lambda shim_dir, *, saved_path="": cli.Path("/usr/local/bin/codex"),
    )
    monkeypatch.setattr(cli.setup_flow, "write_codex_shim", fake_write_codex_shim)
    monkeypatch.setattr(cli.setup_flow, "is_setup_complete", lambda state: True)
    monkeypatch.setattr(cli.shell_integration, "install_path_block", lambda zprofile_path, shim_dir: None)
    monkeypatch.setattr(cli.shell_integration, "has_path_block", lambda zprofile_path: True)
    monkeypatch.setattr(cli, "_resolve_selected_device", fake_resolve_selected_device)
    monkeypatch.setattr(cli, "_pair_selected_device", fake_pair_selected_device)
    monkeypatch.setattr(cli, "_install_launchd_service", lambda state_path: None)
    monkeypatch.setattr(cli, "launchd_service_status", lambda label: {"loaded": True})

    exit_code = asyncio.run(cli._setup(argparse.Namespace(state_path=state_path), repair=True))

    saved = cli.BridgeStateStore(state_path).load()
    assert exit_code == 0
    assert saved.real_codex_path == "/usr/local/bin/codex"
    assert saved.codex_launch_path == "/custom/node/bin:/usr/bin:/bin"


def test_status_prefers_live_agent_status(monkeypatch, capsys):
    def fake_agent_status(state_path):
        assert state_path == "/tmp/codebuddy-state.json"
        return {
            "ok": True,
            "state": {
                "agent_running": True,
                "buddy_connected": True,
                "snapshot": {"total": 1, "running": 1, "waiting": 0, "msg": "working"},
            },
        }

    monkeypatch.setattr(cli, "_agent_status", fake_agent_status)

    exit_code = cli._status(argparse.Namespace(state_path="/tmp/codebuddy-state.json"))

    assert exit_code == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["agent_running"] is True
    assert payload["snapshot"]["msg"] == "working"


def test_help_only_surfaces_public_user_commands(capsys):
    with pytest.raises(SystemExit) as exc_info:
        cli.main(["--help"])

    assert exc_info.value.code == 0
    output = capsys.readouterr().out
    assert "doctor" in output
    assert "repair" in output
    assert "uninstall" in output
    assert "firmware" in output
    assert "agent" not in output
    assert "service-install" not in output
    assert "sessions" not in output


def test_firmware_update_parser_accepts_default_and_explicit_application_image(tmp_path):
    parser = cli.build_parser()
    default = parser.parse_args(["firmware", "update"])
    explicit = parser.parse_args(
        ["firmware", "update", "--firmware", str(tmp_path / "firmware.bin")]
    )

    assert default.command == "firmware"
    assert default.firmware_command == "update"
    assert default.firmware is None
    assert explicit.firmware == tmp_path / "firmware.bin"


def test_firmware_update_reports_sanitized_agent_progress(monkeypatch, tmp_path, capsys):
    image = tmp_path / "firmware.bin"
    image.write_bytes(b"not-inspected-by-cli")
    responses = iter(
        [
            {"ok": True, "ota": {"nonce": "secret-nonce", "phase": "preparing", "terminal": False}},
            {"ok": True, "ota": {"nonce": "secret-nonce", "phase": "await-confirm", "percent": 0, "terminal": False}},
            {"ok": True, "ota": {"nonce": "secret-nonce", "phase": "readback", "percent": 90, "terminal": False}},
            {"ok": True, "ota": {"nonce": "secret-nonce", "phase": "running", "percent": 100, "terminal": True, "success": True, "version": "0.1.5"}},
        ]
    )

    async def fake_ensure(_):
        return None

    async def fake_request(_, payload):
        if payload["cmd"] == "ota_begin":
            assert payload["firmware"] == str(image)
        return next(responses)

    async def no_sleep(_):
        return None

    monkeypatch.setattr(cli, "_ensure_agent_running", fake_ensure)
    monkeypatch.setattr(cli, "_agent_request", fake_request)
    monkeypatch.setattr(cli.asyncio, "sleep", no_sleep)

    result = asyncio.run(
        cli._firmware_update(
            argparse.Namespace(state_path=tmp_path / "state.json", firmware=image)
        )
    )

    output = capsys.readouterr().out
    assert result == 0
    assert "Press A" in output
    assert "90%" in output
    assert "0.1.5" in output
    assert "secret-nonce" not in output


def test_firmware_update_sends_cancel_on_keyboard_interrupt(monkeypatch, tmp_path):
    requests: list[dict] = []

    async def fake_ensure(_):
        return None

    async def fake_request(_, payload):
        requests.append(payload)
        if payload["cmd"] == "ota_begin":
            return {"ok": True, "ota": {"nonce": "n", "phase": "preparing", "terminal": False}}
        if payload["cmd"] == "ota_status":
            raise KeyboardInterrupt
        return {"ok": True}

    monkeypatch.setattr(cli, "_ensure_agent_running", fake_ensure)
    monkeypatch.setattr(cli, "_agent_request", fake_request)

    result = asyncio.run(
        cli._firmware_update(
            argparse.Namespace(state_path=tmp_path / "state.json", firmware=None)
        )
    )

    assert result == 130
    assert requests[-1] == {"cmd": "ota_cancel", "nonce": "n"}


def test_version_flag_reports_project_version(capsys):
    with pytest.raises(SystemExit) as exc_info:
        cli.main(["--version"])

    assert exc_info.value.code == 0
    assert capsys.readouterr().out.strip() == f"code-buddy {_project_version()}"
