from __future__ import annotations

import asyncio
import contextlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Optional

from . import runtime
from .ota_protocol import OtaProtocolError, build_ota_offer, parse_ota_status
from .ota_release import OtaImageInfo, build_ota_release
from .ota_server import OtaHttpsServer
from .ota_trust import require_existing_ota_trust
from .ble_transport import NativeBleHelperError


_DEVICE_ERRORS = {
    "busy", "cancelled", "conflict", "download", "hash", "manifest",
    "power", "reconnect", "rejected", "rollback", "timeout", "trust",
    "version", "wifi",
}


@dataclass
class OtaAgentSession:
    nonce: str
    generation: int
    image_path: Path
    version: str
    phase: str = "preparing"
    percent: int = 0
    terminal: bool = False
    success: bool = False
    error: str = ""
    health: str = ""
    accepted: bool = False
    irreversible: bool = False
    cancel_event: asyncio.Event = field(default_factory=asyncio.Event)
    cancel_result: Optional[asyncio.Future[bool]] = field(
        default=None, init=False, repr=False
    )
    cancel_sent: bool = field(default=False, init=False)
    cancel_complete: asyncio.Event = field(default_factory=asyncio.Event)

    def public_payload(self, *, include_identity: bool = True) -> dict[str, object]:
        payload: dict[str, object] = {
            "phase": self.phase,
            "percent": self.percent,
            "terminal": self.terminal,
            "success": self.success,
            "version": self.version,
            "health": self.health,
        }
        if include_identity:
            payload["nonce"] = self.nonce
            payload["generation"] = self.generation
        if self.error:
            payload["error"] = self.error
        return payload


class OtaCoordinator:
    def __init__(
        self,
        *,
        get_ble: Callable[[], Any],
        is_ble_connected: Callable[[], bool],
        ota_session: Callable[[], Any],
        trust_loader: Callable[[], Any] = require_existing_ota_trust,
        server_factory: Callable[..., Any] = OtaHttpsServer,
        release_builder: Callable[..., Any] = build_ota_release,
        status_timeout: float = 15.0,
        confirm_timeout: float = 180.0,
        install_timeout: float = 600.0,
        reconnect_interval: float = 5.0,
        cancel_timeout: float = 1.5,
    ) -> None:
        self._get_ble = get_ble
        self._is_ble_connected = is_ble_connected
        self._ota_session = ota_session
        self._trust_loader = trust_loader
        self._server_factory = server_factory
        self._release_builder = release_builder
        self._status_timeout = status_timeout
        self._confirm_timeout = confirm_timeout
        self._install_timeout = install_timeout
        self._reconnect_interval = reconnect_interval
        self._cancel_timeout = cancel_timeout

    async def cancel(self, session: OtaAgentSession) -> bool:
        if session.phase == "cancelled" and session.cancel_complete.is_set():
            return True
        if session.terminal or session.irreversible or session.phase in {
            "boot-committed", "restarting", "boot-health"
        }:
            return False
        if session.cancel_result is None or session.cancel_result.done():
            session.cancel_result = asyncio.get_running_loop().create_future()
        session.cancel_event.set()
        try:
            applied = await asyncio.wait_for(
                asyncio.shield(session.cancel_result),
                timeout=self._cancel_timeout,
            )
        except asyncio.TimeoutError:
            return False
        if not applied:
            return False
        try:
            await asyncio.wait_for(
                session.cancel_complete.wait(), timeout=self._cancel_timeout
            )
        except asyncio.TimeoutError:
            return False
        return True

    @staticmethod
    def _finish_cancel_request(session: OtaAgentSession, applied: bool) -> None:
        pending = session.cancel_result
        session.cancel_sent = False
        session.cancel_event.clear()
        if applied:
            session.phase = "cancelled"
            session.error = "cancelled"
            session.terminal = True
            session.success = False
        if pending is not None and not pending.done():
            pending.set_result(applied)

    async def _send_cancel_request(self, session: OtaAgentSession) -> None:
        if not session.cancel_event.is_set() or session.cancel_sent:
            return
        ble = self._get_ble()
        if ble is None or not self._is_ble_connected():
            self._finish_cancel_request(session, False)
            return
        try:
            await ble.send_json(
                {
                    "cmd": "ota_cancel",
                    "nonce": session.nonce,
                    "generation": session.generation,
                }
            )
        except Exception:
            self._finish_cancel_request(session, False)
            return
        session.cancel_sent = True
        session.cancel_event.clear()

    async def _receive(self, session: OtaAgentSession, *, timeout: float):
        deadline = asyncio.get_running_loop().time() + timeout
        while True:
            await self._send_cancel_request(session)
            remaining = deadline - asyncio.get_running_loop().time()
            if remaining <= 0:
                raise asyncio.TimeoutError
            ble = self._get_ble()
            if ble is None or not self._is_ble_connected():
                await asyncio.sleep(min(0.1, remaining))
                continue
            try:
                raw = await ble.recv_json(timeout=min(0.05, remaining))
            except asyncio.TimeoutError:
                continue
            try:
                status = parse_ota_status(
                    raw, nonce=session.nonce, generation=session.generation
                )
            except OtaProtocolError:
                continue
            if session.cancel_sent and (
                status.cancel_applied
                or status.phase in {
                    "boot-committed", "restarting", "boot-health", "running",
                    "rejected", "busy", "error",
                }
            ):
                self._finish_cancel_request(session, status.cancel_applied)
            if status.phase == "cancelled" and not status.cancel_applied:
                await self._probe(session)
                continue
            return status

    async def _probe(self, session: OtaAgentSession) -> None:
        ble = self._get_ble()
        if ble is None or not self._is_ble_connected():
            raise RuntimeError("reconnect")
        await ble.send_json(
            {
                "cmd": "ota_status",
                "nonce": session.nonce,
                "generation": session.generation,
            }
        )

    async def run(self, session: OtaAgentSession, inspected: OtaImageInfo) -> None:
        server = None
        try:
            async with self._ota_session():
                session.phase = "preparing"
                await self._probe(session)
                current = await self._receive(session, timeout=self._status_timeout)
                if current.phase != "running" or not current.version:
                    raise RuntimeError("version")

                trust = self._trust_loader()

                def release_factory(firmware_url: str):
                    return self._release_builder(
                        image_path=inspected.path,
                        output_dir=runtime.ota_releases_dir() / "current",
                        version=inspected.version,
                        current_version=current.version,
                        chip="esp32s3",
                        artifact_url=firmware_url,
                        signing_private_key=trust.manifest_private_key,
                        expected_signing_public_key=trust.manifest_public_key,
                        expected_size_bytes=inspected.size_bytes,
                        expected_sha256=inspected.sha256,
                    )

                server = self._server_factory(
                    release_factory=release_factory,
                    trust=trust,
                )
                endpoint = await server.start()
                offer = build_ota_offer(
                    nonce=session.nonce,
                    generation=session.generation,
                    version=inspected.version,
                    size_bytes=inspected.size_bytes,
                    manifest_url=endpoint.manifest_url,
                    signature_url=endpoint.signature_url,
                )

                session.phase = "await-confirm"
                accepted = False
                for _ in range(3):
                    ble = self._get_ble()
                    if ble is None:
                        raise RuntimeError("reconnect")
                    await ble.send_json(offer)
                    try:
                        status = await self._receive(session, timeout=2.0)
                    except asyncio.TimeoutError:
                        continue
                    if status.phase in {"offer-received", "await-confirm"}:
                        break
                    if status.phase in {
                        "accepted", "authenticated", "download", "readback",
                        "boot-committed", "restarting", "boot-health", "running",
                    }:
                        accepted = session.accepted = True
                        session.phase = status.phase
                        session.percent = status.percent
                        if status.phase in {
                            "boot-committed", "restarting", "boot-health"
                        }:
                            session.irreversible = True
                        break
                    if status.phase in {"rejected", "busy", "error"}:
                        raise RuntimeError(status.error or status.phase)
                else:
                    raise RuntimeError("timeout")

                deadline = asyncio.get_running_loop().time() + self._install_timeout
                awaiting_restart_health = session.irreversible
                recovering_transport = False
                while True:
                    remaining = deadline - asyncio.get_running_loop().time()
                    if remaining <= 0:
                        raise asyncio.TimeoutError
                    if recovering_transport:
                        try:
                            await self._probe(session)
                        except (NativeBleHelperError, RuntimeError):
                            await asyncio.sleep(
                                max(0, min(self._reconnect_interval, remaining))
                            )
                            continue
                        recovering_transport = False
                    if awaiting_restart_health:
                        with contextlib.suppress(Exception):
                            await self._probe(session)
                    receive_timeout = min(
                        self._confirm_timeout if not accepted else remaining,
                        remaining,
                    )
                    if awaiting_restart_health:
                        receive_timeout = min(
                            receive_timeout,
                            max(0.05, min(1.0, self._reconnect_interval)),
                        )
                    try:
                        status = await self._receive(
                            session, timeout=receive_timeout
                        )
                    except NativeBleHelperError:
                        if accepted or session.accepted or session.irreversible:
                            if session.irreversible:
                                awaiting_restart_health = True
                            recovering_transport = True
                            await asyncio.sleep(
                                max(0, min(self._reconnect_interval, remaining))
                            )
                            continue
                        raise
                    except asyncio.TimeoutError:
                        if awaiting_restart_health:
                            continue
                        raise
                    session.phase = status.phase
                    session.percent = status.percent
                    session.health = status.health
                    if status.phase == "accepted":
                        accepted = session.accepted = True
                    elif status.phase in {"rejected", "busy", "cancelled", "error"}:
                        raise RuntimeError(status.error or status.phase)
                    elif status.phase == "running":
                        if status.version == inspected.version and status.health == "valid":
                            session.percent = 100
                            session.success = session.terminal = True
                            return
                        if accepted and not session.irreversible:
                            raise RuntimeError("rollback")
                        if session.irreversible:
                            awaiting_restart_health = True
                    elif status.phase in {
                        "boot-committed", "restarting", "boot-health"
                    }:
                        session.irreversible = True
                        awaiting_restart_health = True
                        if status.phase == "restarting":
                            await asyncio.sleep(self._reconnect_interval)
        except asyncio.CancelledError:
            session.phase, session.error = "cancelled", "cancelled"
            session.terminal = True
        except asyncio.TimeoutError:
            session.phase, session.error = "error", "timeout"
            session.terminal = True
        except Exception as exc:
            code = str(exc) if str(exc) in _DEVICE_ERRORS else "download"
            session.phase = "cancelled" if code == "cancelled" else "error"
            session.error = code
            session.terminal = True
        finally:
            if server is not None:
                with contextlib.suppress(Exception):
                    await server.close()
            if session.cancel_result is not None and not session.cancel_result.done():
                self._finish_cancel_request(session, False)
            if session.phase == "cancelled" and session.terminal:
                session.cancel_complete.set()
