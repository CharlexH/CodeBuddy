from __future__ import annotations

import asyncio
import os
import signal
import subprocess
import sys
from pathlib import Path

import pytest

from codex_buddy.agent import AgentClient, BuddyAgent
from codex_buddy.agent_runtime import (
    AgentProcessLock,
    cleanup_stale_ota_runtime,
)


def test_process_lock_is_exclusive_until_owner_releases_it(tmp_path):
    path = tmp_path / "agent.lock"
    owner = AgentProcessLock(path)
    contender = AgentProcessLock(path)

    owner.acquire()
    with pytest.raises(RuntimeError, match="already running"):
        contender.acquire()

    owner.release()
    contender.acquire()
    contender.release()


def test_concurrent_agent_start_preserves_the_live_socket_and_ping_owner(tmp_path):
    async def exercise():
        socket = Path(f"/tmp/code-buddy-agent-lock-{id(tmp_path)}.sock")
        second_monitor_started = False
        first = BuddyAgent(
            tmp_path / "state.json",
            socket_path=socket,
            watcher=None,
            readonly_poll_interval=60,
            keepalive_interval=60,
            reconnect_interval=60,
        )
        second = BuddyAgent(
            tmp_path / "state.json",
            socket_path=socket,
            watcher=None,
            readonly_poll_interval=60,
            keepalive_interval=60,
            reconnect_interval=60,
        )

        async def record_monitor_start():
            nonlocal second_monitor_started
            second_monitor_started = True

        second._start_account_usage_monitor = record_monitor_start
        first_task = asyncio.create_task(first.run())
        try:
            for _ in range(100):
                if first._server is not None:
                    break
                await asyncio.sleep(0.001)
            assert first._server is not None

            with pytest.raises(RuntimeError, match="already running"):
                await second.run()

            assert second_monitor_started is False
            assert socket.exists()
            assert await AgentClient(socket).request({"cmd": "ping"}) == {"ok": True}
            assert await AgentClient(socket).request({"cmd": "stop"}) == {"ok": True}
            await first_task
        finally:
            if not first_task.done():
                first._stop_requested = True
                if first._stopped is not None:
                    first._stopped.set()
                await first_task
            socket.unlink(missing_ok=True)

    asyncio.run(exercise())


def _write_valid_transient_tree(root: Path) -> tuple[Path, Path, Path, Path]:
    snapshots = root / "private" / "snapshots"
    sessions = root / "private" / "sessions"
    releases = root / "releases"
    snapshots.mkdir(parents=True)
    sessions.mkdir(parents=True)
    generations = releases / ".current.generations"
    generations.mkdir(parents=True)

    snapshot = snapshots / ".snapshot-dead"
    snapshot.mkdir()
    (snapshot / "firmware.bin").write_bytes(b"firmware")

    tls = sessions / "https-dead"
    tls.mkdir()
    (tls / "leaf-key.pem").write_text("key")
    (tls / "leaf-cert.pem").write_text("cert")

    current_generation = generations / "1.2.3-abc"
    current_generation.mkdir()
    for name in ("firmware.bin", "manifest.json", "manifest.sig"):
        (current_generation / name).write_text(name)
    (releases / "current").symlink_to(
        Path(".current.generations") / current_generation.name
    )

    staging = generations / ".staging-dead"
    staging.mkdir()
    (staging / "firmware.bin").write_text("partial")
    pointer = releases / ".current.current-dead"
    pointer.symlink_to(Path(".current.generations") / current_generation.name)
    return snapshots, sessions, releases, current_generation


def test_startup_cleanup_removes_only_transient_ota_residue(tmp_path):
    snapshots, sessions, releases, generation = _write_valid_transient_tree(tmp_path)
    stable_firmware = tmp_path / "firmware" / "code-buddy-sticks3-app.bin"
    stable_firmware.parent.mkdir()
    stable_firmware.write_bytes(b"keep")

    cleanup_stale_ota_runtime(
        snapshots_root=snapshots,
        sessions_root=sessions,
        releases_root=releases,
    )

    assert list(snapshots.iterdir()) == []
    assert list(sessions.iterdir()) == []
    assert generation.is_dir()
    assert (releases / "current").resolve() == generation.resolve()
    assert not (generation.parent / ".staging-dead").exists()
    assert not (releases / ".current.current-dead").exists()
    assert stable_firmware.read_bytes() == b"keep"


def test_startup_cleanup_accepts_atomic_write_residue_inside_staging(tmp_path):
    releases = tmp_path / "releases"
    staging = releases / ".current.generations" / ".staging-dead"
    staging.mkdir(parents=True)
    (staging / ".firmware.bin.random").write_bytes(b"partial")

    cleanup_stale_ota_runtime(
        snapshots_root=tmp_path / "snapshots",
        sessions_root=tmp_path / "sessions",
        releases_root=releases,
    )

    assert not staging.exists()


def test_startup_cleanup_refuses_snapshot_symlink_escape(tmp_path):
    outside = tmp_path / "outside"
    outside.mkdir()
    marker = outside / "firmware.bin"
    marker.write_bytes(b"do-not-delete")
    snapshots = tmp_path / "snapshots"
    snapshots.mkdir()
    (snapshots / ".snapshot-escape").symlink_to(outside, target_is_directory=True)

    with pytest.raises(ValueError, match="symlink"):
        cleanup_stale_ota_runtime(
            snapshots_root=snapshots,
            sessions_root=tmp_path / "sessions",
            releases_root=tmp_path / "releases",
        )

    assert marker.read_bytes() == b"do-not-delete"


def test_startup_cleanup_refuses_temporary_release_pointer_escape(tmp_path):
    releases = tmp_path / "releases"
    generations = releases / ".current.generations"
    generations.mkdir(parents=True)
    outside = tmp_path / "outside"
    outside.mkdir()
    (releases / ".current.current-dead").symlink_to(Path("..") / "outside")

    with pytest.raises(ValueError, match="escapes"):
        cleanup_stale_ota_runtime(
            snapshots_root=tmp_path / "snapshots",
            sessions_root=tmp_path / "sessions",
            releases_root=releases,
        )

    assert outside.is_dir()


def test_sigkill_releases_lock_and_next_owner_cleans_valid_residue(tmp_path):
    lock = tmp_path / "agent.lock"
    snapshots = tmp_path / "snapshots"
    ready = tmp_path / "ready"
    script = """
import os
from pathlib import Path
from codex_buddy.agent_runtime import AgentProcessLock
lock = AgentProcessLock(Path(os.environ['LOCK']))
lock.acquire()
snapshot = Path(os.environ['SNAPSHOTS']) / '.snapshot-killed'
snapshot.mkdir(parents=True)
(snapshot / 'firmware.bin').write_bytes(b'partial')
Path(os.environ['READY']).write_text('ready')
signal_pause = __import__('signal').pause
signal_pause()
"""
    environment = dict(os.environ)
    environment.update(LOCK=str(lock), SNAPSHOTS=str(snapshots), READY=str(ready))
    process = subprocess.Popen([sys.executable, "-c", script], env=environment)
    try:
        for _ in range(200):
            if ready.exists():
                break
            import time

            time.sleep(0.005)
        assert ready.exists()
        os.kill(process.pid, signal.SIGKILL)
        assert process.wait(timeout=5) == -signal.SIGKILL

        next_owner = AgentProcessLock(lock)
        next_owner.acquire()
        try:
            cleanup_stale_ota_runtime(
                snapshots_root=snapshots,
                sessions_root=tmp_path / "sessions",
                releases_root=tmp_path / "releases",
            )
        finally:
            next_owner.release()
        assert list(snapshots.iterdir()) == []
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
