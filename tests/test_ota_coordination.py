from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path
from types import SimpleNamespace

from codex_buddy.ota_coordination import OtaAgentSession, OtaCoordinator
from codex_buddy.ble_transport import NativeBleHelperError
from codex_buddy.ota_release import OtaImageInfo


NONCE = "n" * 24


def _status(phase, *, version="0.1.5", percent=0, health="ordinary", error="", nonce=NONCE):
    return {
        "cmd": "ota_status",
        "nonce": nonce,
        "generation": 1,
        "phase": phase,
        "percent": percent,
        "version": version,
        "health": health,
        "error": error,
        "cancel_applied": False,
    }


class _Ble:
    def __init__(self, statuses):
        self.statuses = asyncio.Queue()
        for status in statuses:
            self.statuses.put_nowait(status)
        self.sent = []

    async def send_json(self, payload):
        self.sent.append(payload)

    async def recv_json(self, *, timeout):
        return await asyncio.wait_for(self.statuses.get(), timeout=timeout)


class _Server:
    instances = []

    def __init__(self, *, release_factory, trust):
        self.release = release_factory(
            "https://192.168.1.8:4444/token-token-token-token-1234/firmware.bin"
        )
        self.closed = False
        self.instances.append(self)

    async def start(self):
        return SimpleNamespace(
            manifest_url="https://192.168.1.8:4444/token-token-token-token-1234/manifest.json",
            signature_url="https://192.168.1.8:4444/token-token-token-token-1234/manifest.sig",
        )

    async def close(self):
        self.closed = True


def test_coordination_ignores_foreign_status_and_requires_version_plus_health(tmp_path):
    async def exercise():
        statuses = [
            _status("running", version="9.9.9", nonce="x" * 24),
            _status("running", version="0.1.4", health="valid"),
            _status("offer-received", version="0.1.4"),
            _status("accepted"),
            _status("authenticated"),
            _status("download", percent=40),
            _status("readback", percent=90),
            _status("boot-committed", percent=100),
            _status("restarting", percent=100),
            _status("boot-health", percent=100, health="monitoring"),
            _status("running", percent=100, health="valid"),
        ]
        ble = _Ble(statuses)
        builds = []
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        def build(**kwargs):
            builds.append(kwargs)
            return SimpleNamespace()

        @asynccontextmanager
        async def lock():
            yield

        coordinator = OtaCoordinator(
            get_ble=lambda: ble,
            is_ble_connected=lambda: True,
            ota_session=lock,
            trust_loader=lambda: trust,
            server_factory=_Server,
            release_builder=build,
            reconnect_interval=0,
        )
        image = OtaImageInfo(
            path=tmp_path / "firmware.bin",
            version="0.1.5",
            size_bytes=1234,
            sha256="a" * 64,
        )
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble, builds, _Server.instances[-1]

    session, ble, builds, server = asyncio.run(exercise())
    assert session.success is True and session.terminal is True
    assert session.version == "0.1.5" and session.health == "valid"
    assert builds[0]["version"] == "0.1.5"
    assert builds[0]["current_version"] == "0.1.4"
    offers = [item for item in ble.sent if "ota_offer" in item]
    assert len(offers) == 1
    assert server.closed is True


def test_coordination_rejection_stops_server_before_transfer(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("rejected", version="0.1.4", error="rejected"),
            ]
        )
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        @asynccontextmanager
        async def lock():
            yield

        coordinator = OtaCoordinator(
            get_ble=lambda: ble,
            is_ble_connected=lambda: True,
            ota_session=lock,
            trust_loader=lambda: trust,
            server_factory=_Server,
            release_builder=lambda **_: SimpleNamespace(),
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, _Server.instances[-1]

    session, server = asyncio.run(exercise())
    assert session.terminal is True and session.success is False
    assert session.error == "rejected"
    assert server.closed is True


def test_coordination_reprobes_after_device_reconnect(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("restarting", percent=100),
            ]
        )
        disconnected_checks = 0
        restart_seen = False

        async def recv_json(*, timeout):
            nonlocal restart_seen
            payload = await _Ble.recv_json(ble, timeout=timeout)
            if payload["phase"] == "restarting":
                restart_seen = True
            return payload

        ble.recv_json = recv_json

        async def send_json(payload):
            await _Ble.send_json(ble, payload)
            probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
            if len(probes) >= 2:
                ble.statuses.put_nowait(
                    _status("running", percent=100, health="valid")
                )

        ble.send_json = send_json

        def connected():
            nonlocal disconnected_checks
            probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
            if restart_seen and len(probes) == 1 and disconnected_checks < 2:
                disconnected_checks += 1
                return False
            return True

        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        @asynccontextmanager
        async def lock():
            yield

        coordinator = OtaCoordinator(
            get_ble=lambda: ble,
            is_ble_connected=connected,
            ota_session=lock,
            trust_loader=lambda: trust,
            server_factory=_Server,
            release_builder=lambda **_: SimpleNamespace(),
            install_timeout=0.3,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble

    session, ble = asyncio.run(exercise())
    assert session.success is True
    assert len([item for item in ble.sent if item.get("cmd") == "ota_status"]) >= 2


def test_boot_commit_immediately_enters_irreversible_reconnect_probe(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("boot-committed", percent=100),
            ]
        )
        disconnected = False
        original_recv = ble.recv_json

        async def recv_json(*, timeout):
            nonlocal disconnected
            if not ble.statuses.empty():
                return await original_recv(timeout=timeout)
            if not disconnected:
                disconnected = True
                raise NativeBleHelperError("native helper disconnected during reboot")
            return await original_recv(timeout=timeout)

        async def send_json(payload):
            await _Ble.send_json(ble, payload)
            probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
            if disconnected and len(probes) >= 2:
                ble.statuses.put_nowait(
                    _status("running", percent=100, health="valid")
                )

        ble.recv_json = recv_json
        ble.send_json = send_json
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        @asynccontextmanager
        async def lock():
            yield

        coordinator = OtaCoordinator(
            get_ble=lambda: ble,
            is_ble_connected=lambda: True,
            ota_session=lock,
            trust_loader=lambda: trust,
            server_factory=_Server,
            release_builder=lambda **_: SimpleNamespace(),
            install_timeout=0.5,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble

    session, ble = asyncio.run(exercise())
    assert session.success is True
    probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
    assert len(probes) >= 2
    assert all(item["nonce"] == NONCE and item["generation"] == 1 for item in probes)


def test_cancel_reports_noop_once_boot_is_committed(tmp_path):
    async def exercise():
        ble = _Ble([])

        @asynccontextmanager
        async def lock():
            yield

        coordinator = OtaCoordinator(
            get_ble=lambda: ble,
            is_ble_connected=lambda: True,
            ota_session=lock,
        )
        session = OtaAgentSession(
            NONCE, 1, tmp_path / "firmware.bin", "0.1.5", phase="boot-committed"
        )
        applied = await coordinator.cancel(session)
        return applied, session, ble

    applied, session, ble = asyncio.run(exercise())
    assert applied is False
    assert session.cancel_event.is_set() is False
    assert ble.sent == []
