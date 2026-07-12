import asyncio
import ipaddress
import ssl
import stat
import subprocess
from pathlib import Path

import pytest

import codex_buddy.ota_server as ota_server_module
from codex_buddy.ota_release import OtaRelease
from codex_buddy.ota_server import (
    OtaHttpsServer,
    OtaRequestRouter,
    create_ephemeral_tls_material,
    select_private_lan_ipv4,
)
from codex_buddy.ota_trust import generate_ota_trust


TOKEN = "test-token-with-enough-entropy"


def _release(tmp_path: Path) -> OtaRelease:
    generation = tmp_path / ".current.generations" / "1.2.3-generation"
    generation.mkdir(parents=True)
    (generation / "manifest.json").write_bytes(b'{"schema":1}')
    (generation / "manifest.sig").write_bytes(b"signature-bytes")
    (generation / "firmware.bin").write_bytes(b"firmware-bytes")
    current = tmp_path / "current"
    current.symlink_to(generation)
    return OtaRelease(
        output_dir=current,
        generation_dir=generation,
        firmware=generation / "firmware.bin",
        manifest=generation / "manifest.json",
        signature=generation / "manifest.sig",
    )


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


def test_ephemeral_leaf_is_p256_ip_scoped_server_certificate_and_private(tmp_path):
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
    assert b"prime256v1" in details
    assert b"IP Address:192.168.44.8" in details
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
    router = OtaRequestRouter(_release(tmp_path), token=TOKEN)

    get_response = router.route("GET", f"/{TOKEN}/manifest.json", {})
    head_response = router.route("HEAD", f"/{TOKEN}/firmware.bin", {})

    assert get_response.status == 200
    assert get_response.body == b'{"schema":1}'
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
    response = OtaRequestRouter(_release(tmp_path), token=TOKEN).route(method, path, headers)

    assert response.status == status
    assert response.body != b"firmware-bytes"
    assert response.completes_offer is False


def test_router_reads_immutable_generation_not_mutable_current_alias(tmp_path):
    release = _release(tmp_path)
    router = OtaRequestRouter(release, token=TOKEN)
    replacement = tmp_path / "replacement"
    replacement.mkdir()
    (replacement / "firmware.bin").write_bytes(b"replacement")
    release.output_dir.unlink()
    release.output_dir.symlink_to(replacement)

    response = router.route("GET", f"/{TOKEN}/firmware.bin", {})

    assert response.body == b"firmware-bytes"
    assert response.completes_offer is True


def test_router_rejects_release_paths_outside_generation(tmp_path):
    release = _release(tmp_path)
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
        OtaRequestRouter(malformed, token=TOKEN)


def test_tls_server_is_one_shot_and_removes_leaf_material(tmp_path):
    async def exercise():
        trust = generate_ota_trust(tmp_path / "trust")
        server = OtaHttpsServer(
            release=_release(tmp_path / "release"),
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
        context = ssl.create_default_context(cafile=str(trust.local_ca_certificate))
        reader, writer = await asyncio.open_connection(
            offer.host,
            offer.port,
            ssl=context,
            server_hostname=offer.host,
        )
        writer.write(
            f"GET /{TOKEN}/firmware.bin HTTP/1.1\r\nHost: {offer.host}\r\nConnection: close\r\n\r\n".encode()
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
            release=_release(tmp_path / "release"),
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
            release=_release(tmp_path / "release-2"),
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
