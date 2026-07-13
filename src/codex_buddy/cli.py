from __future__ import annotations

import argparse
import asyncio
import contextlib
import inspect
import json
import os
import shutil
import subprocess
import sys
from dataclasses import replace
from pathlib import Path

from . import __version__, runtime, setup_flow, shell_integration
from .agent import (
    AgentClient,
    AgentClientError,
    BuddyAgent,
    default_log_dir,
    default_socket_path,
    spawn_agent_process,
    wait_for_agent,
)
from .ble_transport import BleBuddyTransport, NativeBleHelperError, _native_helper_app_path
from .bridge import default_state_path
from .launchd import (
    install_launchd_service,
    launchd_label,
    launchd_plist_path,
    launchd_service_status,
    render_launchd_plist,
    uninstall_launchd_service,
)
from .state_store import BridgeStateStore, PersistedState


class _CodeBuddyArgumentParser(argparse.ArgumentParser):
    def format_help(self) -> str:
        text = super().format_help()
        lines = [line for line in text.splitlines() if "==SUPPRESS==" not in line]
        return "\n".join(lines) + ("\n" if text.endswith("\n") else "")


def build_parser() -> argparse.ArgumentParser:
    parser = _CodeBuddyArgumentParser(
        prog="code-buddy",
        description="Install, pair, and maintain Code Buddy for Codex CLI approvals.",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    parser.add_argument("--state-path", type=Path, default=default_state_path())
    subparsers = parser.add_subparsers(dest="command", metavar="{doctor,repair,firmware,uninstall}")
    parser.set_defaults(command="default", device=None, timeout=4.0)

    doctor = subparsers.add_parser("doctor", help="Diagnose the current Code Buddy setup")
    doctor.add_argument("--json", action="store_true", help="Print raw machine-readable diagnostics")
    subparsers.add_parser("repair", help="Repair or finish the local Code Buddy setup")
    firmware = subparsers.add_parser("firmware", help="Install signed StickS3 firmware updates")
    firmware_commands = firmware.add_subparsers(dest="firmware_command", required=True)
    firmware_update = firmware_commands.add_parser(
        "update", help="Push an application firmware image over local Wi-Fi"
    )
    firmware_update.add_argument(
        "--firmware",
        type=Path,
        help="application firmware.bin (development override)",
    )
    uninstall = subparsers.add_parser("uninstall", help="Remove Code Buddy from this Mac")
    uninstall.add_argument("--yes", action="store_true", help="Skip the interactive confirmation")

    pair = subparsers.add_parser("pair", help=argparse.SUPPRESS)
    pair.add_argument("--device", help="Exact device name to bind to")
    pair.add_argument("--timeout", type=float, default=4.0)

    subparsers.add_parser("agent", help=argparse.SUPPRESS)

    for command_name in ("run", "launch"):
        run = subparsers.add_parser(command_name, help=argparse.SUPPRESS)
        run.add_argument("--cd", dest="workdir", type=Path, required=True)
        run.add_argument("prompt", nargs="?")

    subparsers.add_parser("status", help=argparse.SUPPRESS)
    subparsers.add_parser("sessions", help=argparse.SUPPRESS)
    subparsers.add_parser("service-install", help=argparse.SUPPRESS)
    subparsers.add_parser("service-uninstall", help=argparse.SUPPRESS)
    subparsers.add_parser("service-status", help=argparse.SUPPRESS)
    return parser


async def _pair(args: argparse.Namespace) -> int:
    if await _ota_conflict_active(args.state_path):
        print("A firmware update is active. Wait for it to finish before pairing.", file=sys.stderr)
        return 1
    store = BridgeStateStore(args.state_path)
    matches = await BleBuddyTransport.discover(timeout=args.timeout)
    if args.device:
        matches = [match for match in matches if match.name == args.device]
    if not matches:
        print("No Code Buddy device found. Power on the StickS3 and try again.", file=sys.stderr)
        return 1
    selected = _select_device(matches)
    await _pair_selected_device(store, selected)
    print(f"Paired {selected.name} ({selected.device_id}) and synced time")
    return 0


async def _run(args: argparse.Namespace) -> int:
    store = BridgeStateStore(args.state_path)
    current = store.load()
    if not current.paired_device_id:
        print("No paired device. Run `code-buddy` first.", file=sys.stderr)
        return 1
    await _ensure_agent_running(args.state_path)
    response = await _agent_request(args.state_path, {"cmd": "launch", "workdir": str(args.workdir)})
    proxy_url = str(response["proxy_url"])
    command = [
        "codex",
        "--remote",
        proxy_url,
        "-a",
        "untrusted",
        "-C",
        str(args.workdir),
    ]
    if args.prompt:
        command.append(args.prompt)
    process = await asyncio.create_subprocess_exec(*command, stdin=None, stdout=None, stderr=None)
    return await process.wait()


def _status(args: argparse.Namespace) -> int:
    live = _agent_status(args.state_path)
    if live is not None:
        print(json.dumps(live["state"], indent=2, sort_keys=True))
        return 0
    state = BridgeStateStore(args.state_path).load()
    print(json.dumps(state.__dict__, indent=2, sort_keys=True))
    return 0


def _sessions(args: argparse.Namespace) -> int:
    live = _agent_sessions(args.state_path)
    if live is not None:
        print(json.dumps(live["sessions"], indent=2, sort_keys=True))
        return 0
    state = BridgeStateStore(args.state_path).load()
    print(json.dumps(state.sessions, indent=2, sort_keys=True))
    return 0


def _doctor(args: argparse.Namespace) -> int:
    payload = _doctor_payload(args)
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2, sort_keys=True))
        return 0

    print(_render_doctor(payload))
    return 0


def _repair(args: argparse.Namespace) -> int:
    return asyncio.run(_setup(args, repair=True))


def _uninstall(args: argparse.Namespace) -> int:
    if not getattr(args, "yes", False):
        answer = input("Remove Code Buddy from this Mac? [y/N]: ").strip().lower()
        if answer not in {"y", "yes"}:
            print("Cancelled.")
            return 0

    uninstall_launchd_service(launchd_plist_path())
    uninstall_launchd_service(_legacy_launchd_plist_path())
    shell_integration.remove_path_block(runtime.zprofile_path())
    runtime_root = runtime.runtime_root()
    if runtime_root.exists():
        shutil.rmtree(runtime_root)
    legacy_root = runtime.legacy_runtime_root()
    if legacy_root.exists():
        shutil.rmtree(legacy_root)
    print(f"Removed Code Buddy from {runtime_root}")
    return 0


async def _setup(args: argparse.Namespace, *, repair: bool = False) -> int:
    if sys.platform != "darwin":
        print("Code Buddy currently supports macOS only.", file=sys.stderr)
        return 1
    if await _ota_conflict_active(args.state_path):
        print("A firmware update is active. Wait for it to finish before repairing.", file=sys.stderr)
        return 1
    state_path = Path(args.state_path)
    setup_flow.migrate_legacy_state()
    store = BridgeStateStore(state_path)
    current = store.load()

    try:
        helper_app_path = setup_flow.ensure_helper_app_installed()
    except (NativeBleHelperError, subprocess.CalledProcessError, OSError) as exc:
        print(f"Native BLE helper is unavailable: {exc}", file=sys.stderr)
        print("Run `code-buddy repair` after the helper bundle is available.", file=sys.stderr)
        return 1
    setup_flow.ensure_firmware_artifact_installed(
        Path(__file__).resolve().parents[2]
        / "dist" / "firmware" / "code-buddy-sticks3-app.bin"
    )

    try:
        real_codex_path = setup_flow.resolve_real_codex_path(
            runtime.shim_dir(),
            saved_path=current.real_codex_path,
        )
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        print("Install Codex first, then rerun `code-buddy repair`.", file=sys.stderr)
        return 1

    selected = await _resolve_selected_device(args, current)
    if selected is None:
        return 1

    await _pair_selected_device(store, selected)
    setup_flow.write_codex_shim(runtime.shim_path(), python_executable=sys.executable)
    shell_integration.install_path_block(runtime.zprofile_path(), runtime.shim_dir())
    _install_launchd_service(state_path)

    current = store.load()
    next_state = replace(
        current,
        setup_version=setup_flow.SETUP_VERSION,
        real_codex_path=str(real_codex_path),
        codex_launch_path=os.environ.get("PATH", ""),
        helper_app_path=str(helper_app_path),
        shim_dir=str(runtime.shim_dir()),
        shell_integrated=shell_integration.has_path_block(runtime.zprofile_path()),
        service_installed=launchd_service_status(launchd_label())["loaded"],
    )
    store.save(next_state)

    if not setup_flow.is_setup_complete(store.load()):
        print("Code Buddy setup is still incomplete. Run `code-buddy doctor` for details.", file=sys.stderr)
        return 1

    action = "Repaired" if repair else "Installed"
    print(f"{action} Code Buddy.")
    print(f"Device: {selected.name} ({selected.device_id})")
    print(f"Codex shim: {runtime.shim_path()}")
    print("Next: open a new shell, then run `codex` normally.")
    return 0


def _default_firmware_image() -> Path:
    installed = runtime.default_firmware_path()
    if installed.is_file() and not installed.is_symlink():
        return installed
    return Path(__file__).resolve().parents[2] / "dist" / "firmware" / "code-buddy-sticks3-app.bin"


async def _firmware_update(args: argparse.Namespace) -> int:
    image = Path(args.firmware).expanduser() if args.firmware else _default_firmware_image()
    try:
        await _ensure_agent_running(args.state_path)
        response = await _agent_request(
            args.state_path,
            {"cmd": "ota_begin", "firmware": str(image)},
        )
        ota = response["ota"]
        nonce = str(ota["nonce"])
        last_phase = ""
        last_bucket = -1
        while True:
            phase = str(ota.get("phase", ""))
            percent = int(ota.get("percent", 0))
            bucket = percent // 10
            if phase != last_phase or bucket != last_bucket:
                if phase == "await-confirm":
                    print("Update ready. On Code Buddy, Press A to install or B to cancel.")
                elif phase in {"download", "readback"}:
                    label = "Downloading" if phase == "download" else "Verifying flash"
                    print(f"{label}: {percent}%")
                elif phase == "restarting":
                    print("Firmware committed. Waiting for Code Buddy to restart...")
                elif phase == "boot-health":
                    print("Code Buddy restarted. Verifying boot health...")
                elif phase == "preparing":
                    print("Preparing signed local firmware update...")
                last_phase, last_bucket = phase, bucket
            if bool(ota.get("terminal", False)):
                if bool(ota.get("success", False)):
                    print(f"Firmware {ota.get('version', '')} is running and healthy.")
                    return 0
                error = str(ota.get("error", "update-failed"))
                print(f"Firmware update failed: {error}.", file=sys.stderr)
                return 1
            await asyncio.sleep(0.25)
            response = await _agent_request(
                args.state_path,
                {"cmd": "ota_status", "nonce": nonce},
            )
            ota = response["ota"]
    except KeyboardInterrupt:
        if "nonce" in locals():
            with contextlib.suppress(Exception):
                await _agent_request(
                    args.state_path,
                    {"cmd": "ota_cancel", "nonce": nonce},
                )
        print("Firmware update cancelled.", file=sys.stderr)
        return 130
    except (AgentClientError, KeyError, TypeError, ValueError, OSError) as exc:
        message = str(exc)
        if "trust" in message.lower():
            message += " Run `python3 scripts/generate-ota-trust.py` only for the USB trust bootstrap."
        elif "firmware image" in message.lower() or "no such file" in message.lower():
            message += " Build the app image with `scripts/build-firmware-release.sh`."
        print(f"Cannot start firmware update: {message}", file=sys.stderr)
        return 1


def _default_status(args: argparse.Namespace) -> int:
    payload = _doctor_payload(args)
    device_name = payload["paired_device_name"] or "Unknown"
    device_id = payload["paired_device_id"] or "-"
    agent_text = "running" if payload["agent_running"] else "installed"
    print("Code Buddy is ready.")
    print(f"Device: {device_name} ({device_id})")
    print(f"Agent: {agent_text}")
    print(f"Codex shim: {payload['shim_path']}")
    print("Next: run `codex` in a new shell. Use `code-buddy doctor` if anything looks wrong.")
    return 0


async def _agent_command(args: argparse.Namespace) -> int:
    agent = BuddyAgent(Path(args.state_path))
    try:
        await agent.run()
    except KeyboardInterrupt:
        pass
    return 0


def _service_install(args: argparse.Namespace) -> int:
    _install_launchd_service(Path(args.state_path))
    store = BridgeStateStore(args.state_path)
    current = store.load()
    store.save(replace(current, service_installed=True))
    print(f"Installed launchd service at {launchd_plist_path()}")
    return 0


def _service_uninstall(args: argparse.Namespace) -> int:
    uninstall_launchd_service(launchd_plist_path())
    store = BridgeStateStore(args.state_path)
    current = store.load()
    store.save(replace(current, service_installed=False))
    print(f"Removed launchd service at {launchd_plist_path()}")
    return 0


def _service_status(_: argparse.Namespace) -> int:
    print(json.dumps(launchd_service_status(launchd_label()), indent=2, sort_keys=True))
    return 0


def _is_setup_complete(args: argparse.Namespace) -> bool:
    return setup_flow.is_setup_complete(BridgeStateStore(args.state_path).load())


def _doctor_payload(args: argparse.Namespace) -> dict[str, object]:
    state_path = Path(args.state_path)
    state = BridgeStateStore(state_path).load()
    socket_path = default_socket_path(state_path)
    live = _agent_status(state_path)
    launchd_status = launchd_service_status(launchd_label())
    helper_app = state.helper_app_path
    helper_error = None
    if helper_app:
        helper_path = Path(helper_app)
        if not (helper_path / "Contents" / "MacOS" / "CodeBuddyBLEHelper").exists():
            helper_error = f"Helper bundle is missing or incomplete at {helper_path}"
    else:
        try:
            helper_app = str(_native_helper_app_path())
        except (NativeBleHelperError, subprocess.CalledProcessError) as exc:
            helper_error = str(exc)

    shell_integrated = state.shell_integrated and shell_integration.has_path_block(runtime.zprofile_path())
    real_codex_exists = bool(state.real_codex_path) and Path(state.real_codex_path).is_file()
    shim_path = runtime.shim_path() if not state.shim_dir else Path(state.shim_dir) / "codex"

    return {
        "setup_complete": setup_flow.is_setup_complete(
            replace(
                state,
                helper_app_path=str(helper_app or state.helper_app_path),
                shell_integrated=shell_integrated,
                service_installed=state.service_installed and launchd_status["loaded"],
            )
        ),
        "paired_device_id": state.paired_device_id,
        "paired_device_name": state.paired_device_name,
        "agent_socket_path": str(socket_path),
        "agent_running": live is not None,
        "snapshot": live["state"]["snapshot"] if live is not None else state.snapshot,
        "launchd": launchd_status,
        "native_helper_app": helper_app,
        "native_helper_error": helper_error,
        "real_codex_path": state.real_codex_path,
        "codex_launch_path": state.codex_launch_path,
        "real_codex_exists": real_codex_exists,
        "shim_dir": state.shim_dir or str(runtime.shim_dir()),
        "shim_path": str(shim_path),
        "shell_integrated": shell_integrated,
        "service_installed": state.service_installed,
    }


def _render_doctor(payload: dict[str, object]) -> str:
    problems = _doctor_problems(payload)
    lines = []
    if problems:
        lines.append("Code Buddy needs attention.")
        for index, problem in enumerate(problems, start=1):
            lines.append(f"{index}. Problem: {problem['problem']}")
            lines.append(f"   Reason: {problem['reason']}")
            lines.append(f"   Next: {problem['next']}")
    else:
        lines.append("Code Buddy is ready.")
        lines.append(
            "Next: open a new shell and run `codex`. Use `code-buddy repair` if approvals stop showing up."
        )

    lines.append(f"Device: {payload['paired_device_name'] or '-'} ({payload['paired_device_id'] or '-'})")
    lines.append(f"Agent: {'running' if payload['agent_running'] else 'not running'}")
    lines.append(f"Launchd: {'loaded' if payload['launchd']['loaded'] else 'not loaded'}")
    lines.append(f"Shim: {payload['shim_path']}")
    if payload["native_helper_app"]:
        lines.append(f"Helper: {payload['native_helper_app']}")
    return "\n".join(lines)


def _doctor_problems(payload: dict[str, object]) -> list[dict[str, str]]:
    problems: list[dict[str, str]] = []
    if not payload["paired_device_id"]:
        problems.append(
            {
                "problem": "No StickS3 is paired yet.",
                "reason": "Setup never finished a successful `Codex-*` pairing.",
                "next": "Power on the device and run `code-buddy repair`.",
            }
        )
    if not payload["real_codex_exists"]:
        problems.append(
            {
                "problem": "The saved Codex CLI path is missing.",
                "reason": "Code Buddy cannot wrap `codex` unless the real executable still exists.",
                "next": "Reinstall Codex if needed, then run `code-buddy repair`.",
            }
        )
    if payload["native_helper_error"]:
        problems.append(
            {
                "problem": "The native Bluetooth helper is unavailable.",
                "reason": str(payload["native_helper_error"]),
                "next": "Restore the helper bundle, then run `code-buddy repair`.",
            }
        )
    if not payload["shell_integrated"]:
        problems.append(
            {
                "problem": "Shell integration is incomplete.",
                "reason": f"`{runtime.zprofile_path()}` does not contain the managed Code Buddy PATH block.",
                "next": "Run `code-buddy repair`, then open a new shell.",
            }
        )
    if not payload["launchd"]["loaded"]:
        problems.append(
            {
                "problem": "The background agent is not installed or not loaded.",
                "reason": "Launchd is not currently serving `com.codebuddy.agent`.",
                "next": "Run `code-buddy repair` to reinstall the service.",
            }
        )
    return problems


def _select_device(matches) -> object:
    if len(matches) == 1:
        return matches[0]

    print("Multiple Code Buddy devices found:")
    for index, match in enumerate(matches, start=1):
        print(f"{index}. {match.name} ({match.device_id})")

    while True:
        raw = input("Choose a device number: ").strip()
        if raw.isdigit():
            selected_index = int(raw)
            if 1 <= selected_index <= len(matches):
                return matches[selected_index - 1]
        print(f"Enter a number between 1 and {len(matches)}.", file=sys.stderr)


async def _pair_selected_device(store: BridgeStateStore, selected: object) -> None:
    transport = BleBuddyTransport(selected.device_id, device_name=selected.name)
    await transport.connect()
    await transport.send_time_sync()
    await asyncio.sleep(0.25)
    await transport.disconnect()
    current = store.load()
    store.save(
        replace(
            current,
            paired_device_id=selected.device_id,
            paired_device_name=selected.name,
            buddy_connected=False,
        )
    )


async def _resolve_selected_device(args: argparse.Namespace, current: PersistedState):
    if current.paired_device_id and current.paired_device_name:
        return argparse.Namespace(device_id=current.paired_device_id, name=current.paired_device_name)

    matches = await BleBuddyTransport.discover(timeout=getattr(args, "timeout", 4.0))
    if getattr(args, "device", None):
        matches = [match for match in matches if match.name == args.device]
    if not matches:
        print("No Code Buddy device found. Power on the StickS3 and run `code-buddy repair`.", file=sys.stderr)
        return None
    return _select_device(matches)


def _legacy_launchd_plist_path() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / "com.codexbuddy.agent.plist"


def _install_launchd_service(state_path: Path) -> None:
    uninstall_launchd_service(_legacy_launchd_plist_path())
    repo_root = Path(__file__).resolve().parents[2]
    log_dir = default_log_dir(state_path)
    log_dir.mkdir(parents=True, exist_ok=True)
    plist_text = render_launchd_plist(
        python_executable=sys.executable,
        state_path=state_path,
        repo_root=repo_root,
        log_dir=log_dir,
    )
    install_launchd_service(launchd_plist_path(), plist_text)


async def _ensure_agent_running(state_path) -> None:
    state_path = Path(state_path)
    socket_path = default_socket_path(state_path)
    client = AgentClient(socket_path)
    try:
        await client.request({"cmd": "ping"})
        return
    except AgentClientError:
        spawn_agent_process(state_path)
        await wait_for_agent(socket_path)


async def _agent_request(state_path, payload):
    state_path = Path(state_path)
    client = AgentClient(default_socket_path(state_path))
    return await client.request(payload)


def _agent_status(state_path):
    try:
        return asyncio.run(_agent_request(state_path, {"cmd": "status"}))
    except AgentClientError:
        return None


async def _ota_conflict_active(state_path) -> bool:
    try:
        live = await _agent_request(state_path, {"cmd": "status"})
    except AgentClientError:
        return False
    ota = live.get("state", {}).get("ota")
    return isinstance(ota, dict) and not bool(ota.get("terminal", False))


def _agent_sessions(state_path):
    try:
        return asyncio.run(_agent_request(state_path, {"cmd": "sessions"}))
    except AgentClientError:
        return None


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "default":
        if _is_setup_complete(args):
            return _default_status(args)
        result = _setup(args)
        return asyncio.run(result) if inspect.isawaitable(result) else result
    if args.command == "doctor":
        return _doctor(args)
    if args.command == "repair":
        return _repair(args)
    if args.command == "firmware" and args.firmware_command == "update":
        return asyncio.run(_firmware_update(args))
    if args.command == "uninstall":
        return _uninstall(args)
    if args.command == "pair":
        return asyncio.run(_pair(args))
    if args.command == "agent":
        return asyncio.run(_agent_command(args))
    if args.command in {"run", "launch"}:
        return asyncio.run(_run(args))
    if args.command == "status":
        return _status(args)
    if args.command == "sessions":
        return _sessions(args)
    if args.command == "service-install":
        return _service_install(args)
    if args.command == "service-uninstall":
        return _service_uninstall(args)
    if args.command == "service-status":
        return _service_status(args)
    parser.error(f"unknown command: {args.command}")
    return 2
