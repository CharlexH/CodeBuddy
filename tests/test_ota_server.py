import asyncio
import hashlib
import ipaddress
import json
import socket
import ssl
import stat
import subprocess
from pathlib import Path

import pytest

import codex_buddy.ota_server as ota_server_module
from codex_buddy.ota_release import OtaRelease, canonical_manifest_bytes
from codex_buddy.ota_server import (
    OtaEndpointReservation,
    OtaHttpsServer,
    OtaRequestRouter,
    create_ephemeral_tls_material,
    select_private_lan_ipv4,
)
from codex_buddy.ota_trust import generate_ota_trust


TOKEN = "test-token-with-enough-entropy"


def _release(
    tmp_path: Path,
    *,
    artifact_url: str,
    firmware: bytes = b"firmware-bytes",
) -> OtaRelease:
    generation = tmp_path / ".current.generations" / "1.2.3-generation"
    generation.mkdir(parents=True)
    (generation / "manifest.json").write_bytes(
        canonical_manifest_bytes(
            version="1.2.3",
            chip="esp32s3",
            size_bytes=len(firmware),
            sha256=hashlib.sha256(firmware).hexdigest(),
            artifact_url=artifact_url,
        )
    )
    (generation / "manifest.sig").write_bytes(b"signature-bytes")
    (generation / "firmware.bin").write_bytes(firmware)
    current = tmp_path / "current"
    current.symlink_to(generation)
    return OtaRelease(
        output_dir=current,
        generation_dir=generation,
        firmware=generation / "firmware.bin",
        manifest=generation / "manifest.json",
        signature=generation / "manifest.sig",
    )


async def _wait_for_connection_count(server: OtaHttpsServer, expected: int) -> None:
    for _ in range(100):
        if server.active_connection_count == expected:
            return
        await asyncio.sleep(0.001)
    assert server.active_connection_count == expected


class _NonReadingWriter:
    def __init__(self, *, wait_closed_blocks: bool = False) -> None:
        self.closed = False
        self.bytes_written = 0
        self._never_drains = asyncio.Event()
        self._wait_closed_blocks = wait_closed_blocks

    def write(self, contents: bytes) -> None:
        self.bytes_written += len(contents)

    async def drain(self) -> None:
        await self._never_drains.wait()

    def close(self) -> None:
        self.closed = True

    async def wait_closed(self) -> None:
        if self._wait_closed_blocks:
            await asyncio.Event().wait()


def test_private_lan_selection_accepts_only_rfc1918_candidates():
    selected = select_private_lan_ipv4(
        discovery=lambda: [
            "127.0.0.1",
            "169.254.2.3",
            "8.8.8.8",
            "198.18.0.1",
            "192.168.44.8",
            "10.4.5.6",
        ]
    )

    assert selected == "192.168.44.8"


@pytest.mark.parametrize(
    "candidate",
    ["127.0.0.1", "169.254.1.1", "8.8.8.8", "100.64.0.1", "198.18.0.1", "::1", "not-an-ip"],
)
def test_private_lan_selection_fails_closed_without_rfc1918(candidate):
    with pytest.raises(RuntimeError, match="active private LAN IPv4"):
        select_private_lan_ipv4(discovery=lambda: [candidate])


def test_endpoint_token_byte_bounds_match_firmware_parser():
    assert not ota_server_module._TOKEN.fullmatch("x" * 23)
    assert ota_server_module._TOKEN.fullmatch("x" * 127)
    assert not ota_server_module._TOKEN.fullmatch("x" * 128)


def test_ephemeral_leaf_has_exact_ip_and_legacy_dns_sans_and_is_private(tmp_path):
    trust = generate_ota_trust(tmp_path / "trust")
    sessions = tmp_path / "sessions"

    material = create_ephemeral_tls_material(
        trust=trust,
        ip_address="192.168.44.8",
        session_root=sessions,
    )

    assert stat.S_IMODE(material.session_dir.stat().st_mode) == 0o700
    assert stat.S_IMODE(material.private_key.stat().st_mode) == 0o600
    details = subprocess.run(
        ["openssl", "x509", "-in", str(material.certificate), "-noout", "-text"],
        check=True,
        capture_output=True,
    ).stdout
    san_lines = details.decode("utf-8").split("X509v3 Subject Alternative Name:", 1)[1]
    san_line = next(line.strip() for line in san_lines.splitlines() if line.strip())
    san_entries = [
        entry.strip()
        for entry in san_line.split(",")
        if entry.strip()
    ]
    assert b"prime256v1" in details
    assert san_entries == [
        "IP Address:192.168.44.8",
        "DNS:192.168.44.8",
    ]
    assert b"TLS Web Server Authentication" in details
    assert b"CA:FALSE" in details
    subprocess.run(
        [
            "openssl",
            "verify",
            "-CAfile",
            str(trust.local_ca_certificate),
            str(material.certificate),
        ],
        check=True,
        capture_output=True,
    )
    assert subprocess.run(
        [
            "openssl",
            "x509",
            "-in",
            str(material.certificate),
            "-checkend",
            "90000",
            "-noout",
        ],
        capture_output=True,
    ).returncode != 0

    material.cleanup()
    assert not material.session_dir.exists()


def test_leaf_key_path_is_private_before_openssl_writes_it(tmp_path, monkeypatch):
    trust = generate_ota_trust(tmp_path / "trust")
    observed_modes = []
    real_run = subprocess.run

    def observing_run(arguments, **kwargs):
        if len(arguments) > 1 and arguments[1] == "ecparam":
            key_path = Path(arguments[arguments.index("-out") + 1])
            observed_modes.append(stat.S_IMODE(key_path.stat().st_mode))
        return real_run(arguments, **kwargs)

    monkeypatch.setattr(ota_server_module.subprocess, "run", observing_run)

    material = create_ephemeral_tls_material(
        trust=trust,
        ip_address="192.168.44.8",
        session_root=tmp_path / "sessions",
    )

    assert observed_modes == [0o600]
    material.cleanup()


def test_router_serves_exact_allowlist_with_get_and_head(tmp_path):
    firmware_url = f"https://192.168.44.8:443/{TOKEN}/firmware.bin"
    router = OtaRequestRouter(
        _release(tmp_path, artifact_url=firmware_url),
        token=TOKEN,
        expected_firmware_url=firmware_url,
    )

    get_response = router.route("GET", f"/{TOKEN}/manifest.json", {})
    head_response = router.route("HEAD", f"/{TOKEN}/firmware.bin", {})

    assert get_response.status == 200
    assert json.loads(get_response.body)["artifact"]["url"] == firmware_url
    assert get_response.headers["Content-Length"] == str(len(get_response.body))
    assert head_response.status == 200
    assert head_response.body == b""
    assert head_response.headers["Content-Length"] == str(len(b"firmware-bytes"))
    assert head_response.completes_offer is False


@pytest.mark.parametrize(
    ("method", "path", "headers", "status"),
    [
        ("POST", f"/{TOKEN}/firmware.bin", {}, 405),
        ("GET", f"/{TOKEN}/../firmware.bin", {}, 404),
        ("GET", f"/{TOKEN}/%2e%2e/firmware.bin", {}, 404),
        ("GET", f"/{TOKEN}/firmware.bin?x=1", {}, 404),
        ("GET", "/manifest.json", {}, 404),
        ("GET", f"/{TOKEN}/firmware.bin", {"range": "bytes=0-3"}, 416),
        ("GET", f"/{TOKEN}/firmware.bin", {"transfer-encoding": "chunked"}, 400),
        ("GET", f"/{TOKEN}/firmware.bin", {"content-length": "1"}, 400),
    ],
)
def test_router_rejects_methods_paths_ranges_and_request_bodies(
    tmp_path, method, path, headers, status
):
    firmware_url = f"https://192.168.44.8:443/{TOKEN}/firmware.bin"
    response = OtaRequestRouter(
        _release(tmp_path, artifact_url=firmware_url),
        token=TOKEN,
        expected_firmware_url=firmware_url,
    ).route(method, path, headers)

    assert response.status == status
    assert response.body != b"firmware-bytes"
    assert response.completes_offer is False


def test_router_reads_immutable_generation_not_mutable_current_alias(tmp_path):
    firmware_url = f"https://192.168.44.8:443/{TOKEN}/firmware.bin"
    release = _release(tmp_path, artifact_url=firmware_url)
    router = OtaRequestRouter(
        release,
        token=TOKEN,
        expected_firmware_url=firmware_url,
    )
    replacement = tmp_path / "replacement"
    replacement.mkdir()
    (replacement / "firmware.bin").write_bytes(b"replacement")
    release.output_dir.unlink()
    release.output_dir.symlink_to(replacement)

    response = router.route("GET", f"/{TOKEN}/firmware.bin", {})

    assert response.body == b"firmware-bytes"
    assert response.completes_offer is True


def test_router_rejects_release_paths_outside_generation(tmp_path):
    firmware_url = f"https://192.168.44.8:443/{TOKEN}/firmware.bin"
    release = _release(tmp_path, artifact_url=firmware_url)
    outside = tmp_path / "outside.bin"
    outside.write_bytes(b"outside")
    malformed = OtaRelease(
        output_dir=release.output_dir,
        generation_dir=release.generation_dir,
        firmware=outside,
        manifest=release.manifest,
        signature=release.signature,
    )

    with pytest.raises(ValueError, match="immutable generation"):
        OtaRequestRouter(
            malformed,
            token=TOKEN,
            expected_firmware_url=firmware_url,
        )


def test_reservation_holds_ephemeral_port_while_release_is_signed(tmp_path):
    reservation = OtaEndpointReservation.create(token_factory=lambda: TOKEN)
    try:
        assert reservation.offer.port > 0
        release = _release(tmp_path, artifact_url=reservation.offer.firmware_url)
        server = OtaHttpsServer(
            release=release,
            reservation=reservation,
            trust=generate_ota_trust(tmp_path / "trust"),
            session_root=tmp_path / "sessions",
        )
        manifest = json.loads(release.manifest.read_bytes())
        assert manifest["artifact"]["url"] == reservation.offer.firmware_url
        assert server.offer == reservation.offer
    finally:
        reservation.close()


def test_server_rejects_signed_manifest_url_mismatch_and_closes_reservation(tmp_path):
    reservation = OtaEndpointReservation.create(token_factory=lambda: TOKEN)
    wrong_url = f"https://{reservation.offer.host}:9/{TOKEN}/firmware.bin"

    with pytest.raises(ValueError, match="signed manifest artifact URL"):
        OtaHttpsServer(
            release=_release(tmp_path, artifact_url=wrong_url),
            reservation=reservation,
            trust=generate_ota_trust(tmp_path / "trust"),
            session_root=tmp_path / "sessions",
        )

    assert reservation.closed is True


def test_server_constructs_after_closed_loop_and_starts_in_fresh_loop(tmp_path):
    asyncio.run(asyncio.sleep(0))
    server = OtaHttpsServer(
        release_factory=lambda firmware_url: _release(
            tmp_path / "release",
            artifact_url=firmware_url,
        ),
        trust=generate_ota_trust(tmp_path / "trust"),
        token_factory=lambda: TOKEN,
        session_root=tmp_path / "sessions",
    )

    async def exercise():
        offer = await server.start()
        await server.close()
        return offer

    offer = asyncio.run(exercise())

    assert offer.host == server.offer.host
    assert server.session_dir is not None and not server.session_dir.exists()


def test_start_failure_releases_reserved_port_and_private_tls_session(tmp_path):
    async def exercise():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
        )
        offer = server.offer
        trust.local_ca_certificate.write_text("not a certificate")

        with pytest.raises(RuntimeError, match="openssl failed"):
            await server.start()

        rebound = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            rebound.bind((offer.host, offer.port))
        finally:
            rebound.close()
        assert server.active_connection_count == 0
        assert not any((tmp_path / "sessions").glob("https-*"))

    asyncio.run(exercise())


def test_async_context_start_failure_closes_reservation(tmp_path, monkeypatch):
    async def exercise():
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=generate_ota_trust(tmp_path / "trust"),
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
        )
        offer = server.offer

        def fail_tls_material(**kwargs):
            raise RuntimeError("injected OpenSSL failure")

        monkeypatch.setattr(
            ota_server_module,
            "create_ephemeral_tls_material",
            fail_tls_material,
        )

        with pytest.raises(RuntimeError, match="injected OpenSSL failure"):
            async with server:
                pytest.fail("context body must not run")

        rebound = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            rebound.bind((offer.host, offer.port))
        finally:
            rebound.close()
        assert server.active_connection_count == 0

    asyncio.run(exercise())


def test_tls_server_is_one_shot_and_removes_leaf_material(tmp_path):
    async def exercise():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
        )
        offer = await server.start()
        assert ipaddress.ip_address(offer.host).is_private
        assert offer.port > 0
        assert (
            offer.firmware_url
            == f"https://{offer.host}:{offer.port}/{TOKEN}/firmware.bin"
        )
        served_manifest = json.loads(server.release.manifest.read_bytes())
        assert served_manifest["artifact"]["url"] == offer.firmware_url
        context = ssl.create_default_context(cafile=str(trust.local_ca_certificate))
        reader, writer = await asyncio.open_connection(
            offer.host,
            offer.port,
            ssl=context,
            server_hostname=offer.host,
        )
        writer.write(
            (
                f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\n"
                f"Host: {offer.host}\r\nConnection: close\r\n\r\n"
            ).encode()
        )
        await writer.drain()
        raw = await reader.read()
        writer.close()
        await writer.wait_closed()
        assert raw.endswith(b"firmware-bytes")
        await server.wait_until_complete(timeout=2.0)
        session_dir = server.session_dir
        assert session_dir is not None
        assert not session_dir.exists()
        with pytest.raises((ConnectionRefusedError, OSError)):
            await asyncio.open_connection(offer.host, offer.port)
        with pytest.raises(RuntimeError, match="already been started"):
            await server.start()

    asyncio.run(exercise())


def test_tls_server_tears_down_on_timeout_and_cancellation(tmp_path):
    async def exercise_timeout():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
        )
        await server.start()
        session_dir = server.session_dir
        with pytest.raises(asyncio.TimeoutError):
            await server.wait_until_complete(timeout=0.01)
        assert session_dir is not None and not session_dir.exists()

        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release-2",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN + "-2",
            session_root=tmp_path / "sessions",
        )
        await server.start()
        session_dir = server.session_dir
        waiter = asyncio.create_task(server.wait_until_complete(timeout=5.0))
        await asyncio.sleep(0)
        waiter.cancel()
        with pytest.raises(asyncio.CancelledError):
            await waiter
        assert session_dir is not None and not session_dir.exists()

    asyncio.run(exercise_timeout())


def test_close_cancels_slow_client_before_removing_tls_material(tmp_path):
    async def exercise():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
            request_timeout=5.0,
        )
        await server.start()
        reader = asyncio.StreamReader()
        reader.feed_data(
            f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\nHost: device\r\n\r\n".encode()
        )
        writer = _NonReadingWriter()
        client_task = asyncio.create_task(server._handle_connection(reader, writer))
        await _wait_for_connection_count(server, 1)

        await server.close()

        assert client_task.done()
        assert writer.closed is True
        assert server.active_connection_count == 0
        assert server.session_dir is not None and not server.session_dir.exists()

    asyncio.run(exercise())


def test_max_concurrency_rejects_second_client_and_close_leaves_no_tasks(tmp_path):
    async def exercise():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
            request_timeout=2.0,
            max_concurrency=1,
        )
        offer = await server.start()
        context = ssl.create_default_context(cafile=str(trust.local_ca_certificate))
        first_reader, first_writer = await asyncio.open_connection(
            offer.host,
            offer.port,
            ssl=context,
            server_hostname=offer.host,
        )
        first_writer.write(b"GET /")
        await first_writer.drain()
        await _wait_for_connection_count(server, 1)
        second_reader, second_writer = await asyncio.open_connection(
            offer.host,
            offer.port,
            ssl=context,
            server_hostname=offer.host,
        )
        second_writer.write(
            f"GET /{TOKEN}/manifest.json HTTP/1.1\r\nHost: {offer.host}\r\n\r\n".encode()
        )
        await second_writer.drain()

        response = await asyncio.wait_for(second_reader.read(), timeout=1.0)

        assert response.startswith(b"HTTP/1.1 503")
        await server.close()
        assert server.active_connection_count == 0
        assert await asyncio.wait_for(first_reader.read(), timeout=1.0) == b""
        first_writer.close()
        second_writer.close()

    asyncio.run(exercise())


def test_connection_timeout_bounds_drain_and_writer_shutdown_together(tmp_path):
    async def exercise():
        timeout = 0.1
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=generate_ota_trust(tmp_path / "trust"),
            token_factory=lambda: TOKEN,
            request_timeout=0.05,
            minimum_transfer_bytes_per_second=1,
            max_transfer_timeout=timeout,
        )
        reader = asyncio.StreamReader()
        reader.feed_data(
            f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\nHost: device\r\n\r\n".encode()
        )
        writer = _NonReadingWriter(wait_closed_blocks=True)
        started = asyncio.get_running_loop().time()

        await server._handle_connection(reader, writer)

        elapsed = asyncio.get_running_loop().time() - started
        assert elapsed < timeout * 1.6
        assert writer.closed is True
        assert server.active_connection_count == 0
        assert server._complete.is_set() is False
        await server.close()

    asyncio.run(exercise())


def test_slowloris_header_is_still_bounded_by_request_timeout(tmp_path):
    async def exercise():
        timeout = 0.05
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=generate_ota_trust(tmp_path / "trust"),
            token_factory=lambda: TOKEN,
            request_timeout=timeout,
            max_transfer_timeout=1.0,
        )
        reader = asyncio.StreamReader()
        reader.feed_data(b"GET /")
        writer = _NonReadingWriter()
        started = asyncio.get_running_loop().time()

        await server._handle_connection(reader, writer)

        assert asyncio.get_running_loop().time() - started < timeout * 2
        assert writer.closed is True
        await server.close()

    asyncio.run(exercise())


def test_valid_transfer_may_outlive_header_timeout(tmp_path):
    async def exercise():
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=generate_ota_trust(tmp_path / "trust"),
            token_factory=lambda: TOKEN,
            request_timeout=0.05,
            minimum_transfer_bytes_per_second=1,
            max_transfer_timeout=1.0,
        )
        reader = asyncio.StreamReader()
        reader.feed_data(
            f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\nHost: device\r\n\r\n".encode()
        )
        writer = _NonReadingWriter()
        asyncio.get_running_loop().call_later(0.15, writer._never_drains.set)
        started = asyncio.get_running_loop().time()

        await server._handle_connection(reader, writer)

        assert asyncio.get_running_loop().time() - started > 0.1
        assert server._complete.is_set() is True
        await server.close()

    asyncio.run(exercise())


def test_real_tls_slow_firmware_reader_can_outlive_header_timeout(tmp_path):
    async def exercise():
        firmware = b"z" * (2 * 1024 * 1024)
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
                firmware=firmware,
            ),
            trust=trust,
            token_factory=lambda: TOKEN,
            session_root=tmp_path / "sessions",
            request_timeout=0.05,
            minimum_transfer_bytes_per_second=64 * 1024,
            max_transfer_timeout=5.0,
        )
        offer = await server.start()
        context = ssl.create_default_context(cafile=str(trust.local_ca_certificate))
        reader, writer = await asyncio.open_connection(
            offer.host,
            offer.port,
            ssl=context,
            server_hostname=offer.host,
            limit=4096,
        )
        writer.write(
            f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\nHost: {offer.host}\r\n\r\n".encode()
        )
        await writer.drain()
        await asyncio.sleep(0.15)

        raw = await asyncio.wait_for(reader.read(), timeout=5.0)

        assert raw.endswith(firmware)
        await server.wait_until_complete(timeout=1.0)
        writer.close()
        await writer.wait_closed()

    asyncio.run(exercise())


def test_default_transfer_budget_allows_two_megabytes_more_than_five_seconds(
    tmp_path,
):
    firmware = b"z" * (2 * 1024 * 1024)
    server = OtaHttpsServer(
        release_factory=lambda firmware_url: _release(
            tmp_path / "release",
            artifact_url=firmware_url,
            firmware=firmware,
        ),
        trust=generate_ota_trust(tmp_path / "trust"),
        token_factory=lambda: TOKEN,
    )
    response = server._router.route(
        "GET",
        f"/{TOKEN}/firmware.bin",
        {},
    )

    assert 5.0 < server._transfer_timeout(response) <= 300.0
    server._reservation.close()


@pytest.mark.parametrize(
    "limits",
    [
        {"max_transfer_timeout": float("inf")},
        {"max_transfer_timeout": float("nan")},
        {"minimum_transfer_bytes_per_second": float("nan")},
    ],
)
def test_transfer_limits_must_be_finite(tmp_path, limits):
    with pytest.raises(ValueError, match="resource limits"):
        OtaHttpsServer(
            release_factory=lambda firmware_url: _release(
                tmp_path / "release",
                artifact_url=firmware_url,
            ),
            trust=generate_ota_trust(tmp_path / "trust"),
            token_factory=lambda: TOKEN,
            **limits,
        )
