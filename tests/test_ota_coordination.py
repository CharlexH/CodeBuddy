from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path
from types import SimpleNamespace

import pytest

from codex_buddy.ota_coordination import OtaAgentSession, OtaCoordinator
from codex_buddy.ble_transport import NativeBleHelperError
from codex_buddy.ota_release import OtaImageInfo
from codex_buddy.ota_trust import generate_ota_trust


NONCE = "n" * 24


def _status(
    phase,
    *,
    version="0.1.5",
    percent=0,
    health="ordinary",
    error="",
    nonce=NONCE,
    cancel_applied=False,
):
    return {
        "cmd": "ota_status",
        "nonce": nonce,
        "generation": 1,
        "phase": phase,
        "percent": percent,
        "version": version,
        "health": health,
        "error": error,
        "cancel_applied": cancel_applied,
    }


class _Ble:
    def __init__(self, statuses, *, device_name=None):
        self.statuses = asyncio.Queue()
        for status in statuses:
            self.statuses.put_nowait(status)
        self.sent = []
        if device_name is not None:
            self.device_name = device_name

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


def test_stale_running_during_offer_handshake_is_not_acceptance(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("running", version="0.1.4", health="ordinary"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("download", percent=40),
                _status("readback", percent=90),
                _status("boot-committed", percent=100),
                _status("running", percent=100, health="valid"),
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
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble

    session, ble = asyncio.run(exercise())
    assert session.success is True and session.accepted is True
    assert len([item for item in ble.sent if "ota_offer" in item]) == 2


def test_precommit_running_from_current_boot_is_ignored(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("running", version="0.1.4", health="ordinary"),
                _status("download", percent=40),
                _status("readback", percent=90),
                _status("boot-committed", percent=100),
                _status("running", percent=100, health="valid"),
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
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session

    session = asyncio.run(exercise())
    assert session.success is True and session.error == ""


@pytest.mark.parametrize("health", ["ordinary", "valid", "rollback"])
def test_old_running_after_irreversible_commit_is_rollback(tmp_path, health):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("boot-committed", percent=100),
                _status("running", version="0.1.4", percent=100, health=health),
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
            install_timeout=0.05,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session

    session = asyncio.run(exercise())
    assert session.terminal is True and session.success is False
    assert session.error == "rollback"


def test_target_running_waits_for_valid_health_after_irreversible_commit(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("boot-committed", percent=100),
                _status("running", percent=100, health="monitoring"),
                _status("running", percent=100, health="valid"),
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
            install_timeout=0.1,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session

    session = asyncio.run(exercise())
    assert session.success is True and session.health == "valid"


@pytest.mark.parametrize(
    ("current_version", "expected_fields"),
    [
        (
            "0.1.5",
            {"nonce", "generation", "version", "sizeBytes", "manifestUrl", "signatureUrl"},
        ),
        (
            "0.1.6",
            {
                "nonce", "generation", "version", "sizeBytes", "manifestUrl",
                "signatureUrl", "device", "issuedAt", "expiresAt", "authorization",
            },
        ),
    ],
)
def test_coordination_negotiates_legacy_and_signed_offer_shapes(
    tmp_path, current_version, expected_fields
):
    async def exercise():
        target_version = "0.1.7"
        ble = _Ble(
            [
                _status("running", version=current_version, health="valid"),
                _status("offer-received", version=current_version),
                _status("accepted", version=target_version),
                _status("running", version=target_version, percent=100, health="valid"),
            ],
            device_name="Codex-4DAD",
        )
        trust = generate_ota_trust(tmp_path / f"trust-{current_version}")

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
            reconnect_interval=0,
            clock=lambda: 1_700_000_000.75,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", target_version, 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, [item["ota_offer"] for item in ble.sent if "ota_offer" in item]

    session, offers = asyncio.run(exercise())
    assert session.success is True
    assert len(offers) == 1
    assert set(offers[0]) == expected_fields
    if current_version == "0.1.6":
        assert offers[0]["device"] == "Codex-4DAD"
        assert offers[0]["issuedAt"] == 1_700_000_000
        assert offers[0]["expiresAt"] == 1_700_000_120
        assert bytes.fromhex(offers[0]["authorization"])


@pytest.mark.parametrize("device_name", [None, "Codex-4dad", "Other-4DAD"])
def test_modern_coordination_fails_closed_without_canonical_transport_device_name(
    tmp_path, device_name
):
    async def exercise():
        ble = _Ble(
            [_status("running", version="0.1.6", health="valid")],
            device_name=device_name,
        )
        trust = generate_ota_trust(tmp_path / "trust")

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
            clock=lambda: 1_700_000_000,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.7", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble

    session, ble = asyncio.run(exercise())
    assert session.terminal is True and session.success is False
    assert session.error == "trust"
    assert not [item for item in ble.sent if "ota_offer" in item]


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


@pytest.mark.parametrize("disconnect_after", ["accepted", "authenticated", "download", "readback"])
def test_install_reconnects_after_acceptance_without_resending_offer(
    tmp_path, disconnect_after
):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("authenticated"),
                _status("download", percent=40),
                _status("readback", percent=90),
                _status("boot-committed", percent=100),
                _status("running", percent=100, health="valid"),
            ]
        )
        disconnected = False
        inject_disconnect = False
        original_recv = ble.recv_json

        async def recv_json(*, timeout):
            nonlocal disconnected, inject_disconnect
            if inject_disconnect and not disconnected:
                disconnected = True
                raise NativeBleHelperError("transient helper disconnect")
            payload = await original_recv(timeout=timeout)
            inject_disconnect = payload["phase"] == disconnect_after
            return payload

        ble.recv_json = recv_json
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
        return session, ble, _Server.instances[-1]

    session, ble, server = asyncio.run(exercise())
    assert session.success is True
    assert len([item for item in ble.sent if "ota_offer" in item]) == 1
    assert len([item for item in ble.sent if item.get("cmd") == "ota_status"]) >= 2
    assert server.closed is True


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


def test_cancel_is_terminal_only_after_device_applies_it(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("await-confirm", version="0.1.4"),
            ]
        )
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        async def send_json(payload):
            await _Ble.send_json(ble, payload)
            if payload.get("cmd") == "ota_cancel":
                ble.statuses.put_nowait(
                    _status("cancelled", error="cancelled", cancel_applied=True)
                )

        ble.send_json = send_json

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
            cancel_timeout=0.5,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        run = asyncio.create_task(coordinator.run(session, image))
        while session.phase != "await-confirm":
            await asyncio.sleep(0)
        applied = await coordinator.cancel(session)
        await run
        return applied, session, ble, _Server.instances[-1]

    applied, session, ble, server = asyncio.run(exercise())
    assert applied is True, (
        session.phase,
        session.error,
        session.terminal,
        session.cancel_sent,
        session.cancel_result,
        session.cancel_complete.is_set(),
        ble.sent,
    )
    assert session.phase == "cancelled"
    assert session.error == "cancelled"
    assert session.terminal is True and session.success is False
    assert len([item for item in ble.sent if item.get("cmd") == "ota_cancel"]) == 1
    assert server.closed is True


def test_cancel_waits_through_false_cancelled_cadence_for_applied_ack(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("await-confirm", version="0.1.4"),
            ]
        )
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )
        applied_ack_queued = False

        async def send_json(payload):
            nonlocal applied_ack_queued
            await _Ble.send_json(ble, payload)
            if payload.get("cmd") == "ota_cancel":
                ble.statuses.put_nowait(
                    _status("cancelled", error="cancelled", cancel_applied=False)
                )
            elif payload.get("cmd") == "ota_status":
                probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
                if len(probes) >= 2 and not applied_ack_queued:
                    applied_ack_queued = True
                    ble.statuses.put_nowait(
                        _status("cancelled", error="cancelled", cancel_applied=True)
                    )

        ble.send_json = send_json

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
            cancel_timeout=0.2,
            install_timeout=0.5,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        run = asyncio.create_task(coordinator.run(session, image))
        while session.phase != "await-confirm":
            await asyncio.sleep(0)
        applied = await coordinator.cancel(session)
        await run
        return applied, session, ble, _Server.instances[-1]

    applied, session, ble, server = asyncio.run(exercise())
    assert applied is True
    assert session.phase == "cancelled" and session.terminal is True
    assert len([item for item in ble.sent if item.get("cmd") == "ota_status"]) >= 2
    assert server.closed is True


def test_false_cancelled_cadence_can_advance_to_boot_commit_and_healthy_run(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("await-confirm", version="0.1.4"),
            ]
        )
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )
        completion_queued = False

        async def send_json(payload):
            nonlocal completion_queued
            await _Ble.send_json(ble, payload)
            if payload.get("cmd") == "ota_cancel":
                ble.statuses.put_nowait(
                    _status("cancelled", error="cancelled", cancel_applied=False)
                )
            elif payload.get("cmd") == "ota_status":
                probes = [item for item in ble.sent if item.get("cmd") == "ota_status"]
                if len(probes) >= 2 and not completion_queued:
                    completion_queued = True
                    ble.statuses.put_nowait(_status("boot-committed", percent=100))
                    ble.statuses.put_nowait(
                        _status("running", percent=100, health="valid")
                    )

        ble.send_json = send_json

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
            cancel_timeout=0.2,
            install_timeout=0.5,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        run = asyncio.create_task(coordinator.run(session, image))
        while session.phase != "await-confirm":
            await asyncio.sleep(0)
        applied = await coordinator.cancel(session)
        await run
        return applied, session

    applied, session = asyncio.run(exercise())
    assert applied is False
    assert session.success is True and session.terminal is True
    assert session.phase == "running" and session.health == "valid"


def test_unsolicited_false_cancelled_cadence_reprobes_until_install_deadline(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("accepted"),
                _status("cancelled", error="cancelled", cancel_applied=False),
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
            status_timeout=0.01,
            install_timeout=0.03,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        await coordinator.run(session, image)
        return session, ble, _Server.instances[-1]

    session, ble, server = asyncio.run(exercise())
    assert session.phase == "error" and session.error == "timeout"
    assert len([item for item in ble.sent if item.get("cmd") == "ota_status"]) >= 2
    assert server.closed is True


def test_cancel_rejected_at_boot_commit_keeps_irreversible_session_running(tmp_path):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("await-confirm", version="0.1.4"),
            ]
        )
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        async def send_json(payload):
            await _Ble.send_json(ble, payload)
            if payload.get("cmd") == "ota_cancel":
                ble.statuses.put_nowait(_status("boot-committed", percent=100))
                ble.statuses.put_nowait(
                    _status("running", percent=100, health="valid")
                )

        ble.send_json = send_json

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
            cancel_timeout=0.1,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        run = asyncio.create_task(coordinator.run(session, image))
        while session.phase != "await-confirm":
            await asyncio.sleep(0)
        applied = await coordinator.cancel(session)
        assert applied is False
        assert session.phase != "cancelled" and session.error != "cancelled"
        await run
        return session

    session = asyncio.run(exercise())
    assert session.irreversible is True
    assert session.success is True and session.terminal is True


@pytest.mark.parametrize("failure", ["send", "timeout"])
def test_cancel_send_or_ack_failure_is_unconfirmed_and_run_continues(tmp_path, failure):
    async def exercise():
        ble = _Ble(
            [
                _status("running", version="0.1.4", health="valid"),
                _status("offer-received", version="0.1.4"),
                _status("await-confirm", version="0.1.4"),
            ]
        )
        release_statuses = asyncio.Event()
        trust = SimpleNamespace(
            manifest_private_key=tmp_path / "private.pem",
            manifest_public_key=tmp_path / "public.pem",
        )

        async def send_json(payload):
            await _Ble.send_json(ble, payload)
            if payload.get("cmd") == "ota_cancel":
                if failure == "send":
                    raise NativeBleHelperError("cancel send failed")
                return
            if payload.get("cmd") == "ota_status" and release_statuses.is_set():
                ble.statuses.put_nowait(_status("boot-committed", percent=100))
                ble.statuses.put_nowait(
                    _status("running", percent=100, health="valid")
                )

        ble.send_json = send_json

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
            cancel_timeout=0.02,
            reconnect_interval=0,
        )
        image = OtaImageInfo(tmp_path / "firmware.bin", "0.1.5", 1234, "a" * 64)
        session = OtaAgentSession(NONCE, 1, image.path, image.version)
        run = asyncio.create_task(coordinator.run(session, image))
        while session.phase != "await-confirm":
            await asyncio.sleep(0)
        applied = await coordinator.cancel(session)
        assert applied is False
        assert session.phase != "cancelled" and session.error != "cancelled"
        release_statuses.set()
        await ble.send_json({"cmd": "ota_status"})
        await run
        return session

    session = asyncio.run(exercise())
    assert session.success is True
