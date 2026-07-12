from __future__ import annotations

import asyncio
import contextlib
import ipaddress
import json
import math
import os
import re
import secrets
import shutil
import socket
import ssl
import stat
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Iterable, Mapping, Optional

from .ota_release import OtaRelease
from .ota_trust import OtaTrustPaths
from .runtime import ota_sessions_dir


_RFC1918_NETWORKS = tuple(
    ipaddress.ip_network(network)
    for network in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")
)
_TOKEN = re.compile(r"^[A-Za-z0-9_-]{24,127}$")
_HEADER_NAME = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")
_STATUS_PHRASES = {
    200: "OK",
    400: "Bad Request",
    404: "Not Found",
    405: "Method Not Allowed",
    408: "Request Timeout",
    416: "Range Not Satisfiable",
    431: "Request Header Fields Too Large",
    500: "Internal Server Error",
    503: "Service Unavailable",
}


def _discover_active_ipv4() -> Iterable[str]:
    candidates = []
    try:
        completed = subprocess.run(
            ["ifconfig"],
            check=False,
            capture_output=True,
            text=True,
        )
        active_interface = False
        for line in completed.stdout.splitlines():
            if line and not line[0].isspace():
                flags = (
                    line.split("flags=", 1)[1].split(">", 1)[0]
                    if "flags=" in line
                    else ""
                )
                active_interface = (
                    "UP" in flags
                    and "RUNNING" in flags
                    and "LOOPBACK" not in flags
                    and "POINTOPOINT" not in flags
                )
                continue
            stripped = line.strip()
            if active_interface and stripped.startswith("inet "):
                candidates.append(stripped.split()[1])
    except OSError:
        pass
    for destination in (("192.0.2.1", 9), ("198.51.100.1", 9), ("203.0.113.1", 9)):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.connect(destination)
            candidates.append(str(sock.getsockname()[0]))
        except OSError:
            pass
        finally:
            sock.close()
    return candidates


def _is_rfc1918_ipv4(value: str) -> bool:
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        return False
    return isinstance(address, ipaddress.IPv4Address) and any(
        address in network for network in _RFC1918_NETWORKS
    )


def select_private_lan_ipv4(
    *, discovery: Callable[[], Iterable[str]] = _discover_active_ipv4
) -> str:
    for candidate in discovery():
        normalized = str(candidate).strip()
        if _is_rfc1918_ipv4(normalized):
            return normalized
    raise RuntimeError("no active private LAN IPv4 address is available for OTA")


def _run_openssl(arguments: list[str]) -> None:
    try:
        subprocess.run(
            ["openssl", *arguments],
            check=True,
            capture_output=True,
        )
    except FileNotFoundError as exc:
        raise RuntimeError("openssl is required to create the OTA HTTPS certificate") from exc
    except subprocess.CalledProcessError as exc:
        detail = exc.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(
            "openssl failed while creating the OTA HTTPS certificate: "
            + (detail or "unknown error")
        ) from exc


def _require_private_session_root(root: Path) -> None:
    if os.path.lexists(root) and root.is_symlink():
        raise ValueError("OTA TLS session root must not be a symlink")
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    metadata = root.lstat()
    if not stat.S_ISDIR(metadata.st_mode):
        raise ValueError("OTA TLS session root must be a directory")
    root.chmod(0o700)


def _create_private_file(path: Path) -> None:
    flags = os.O_CREAT | os.O_EXCL | os.O_WRONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags, 0o600)
    try:
        os.fchmod(descriptor, 0o600)
    finally:
        os.close(descriptor)


@dataclass(frozen=True)
class EphemeralTlsMaterial:
    session_dir: Path
    private_key: Path
    certificate: Path

    def cleanup(self) -> None:
        if self.session_dir.exists():
            shutil.rmtree(self.session_dir)


def create_ephemeral_tls_material(
    *,
    trust: OtaTrustPaths,
    ip_address: str,
    session_root: Optional[Path] = None,
) -> EphemeralTlsMaterial:
    if not _is_rfc1918_ipv4(ip_address):
        raise ValueError("OTA HTTPS certificate requires an RFC1918 IPv4 address")
    if (
        trust.local_ca_private_key.is_symlink()
        or not trust.local_ca_private_key.is_file()
    ):
        raise ValueError("local OTA CA private key must be a regular file")
    if trust.local_ca_certificate.is_symlink() or not trust.local_ca_certificate.is_file():
        raise ValueError("local OTA CA certificate must be a regular file")

    root = Path(session_root) if session_root is not None else ota_sessions_dir()
    _require_private_session_root(root)
    session = Path(tempfile.mkdtemp(prefix="https-", dir=root))
    session.chmod(0o700)
    private_key = session / "leaf-key.pem"
    certificate = session / "leaf-cert.pem"
    request = session / "leaf.csr"
    extensions = session / "leaf-extensions.cnf"
    try:
        _create_private_file(private_key)
        extensions.write_text(
            "[req]\n"
            "prompt = no\n"
            "distinguished_name = subject\n"
            "[subject]\n"
            "CN = Code Buddy One-Shot OTA\n"
            "[server]\n"
            "basicConstraints = critical,CA:FALSE\n"
            "keyUsage = critical,digitalSignature,keyAgreement\n"
            "extendedKeyUsage = serverAuth\n"
            "subjectKeyIdentifier = hash\n"
            "authorityKeyIdentifier = keyid,issuer\n"
            f"subjectAltName = IP:{ip_address}\n",
            encoding="utf-8",
        )
        extensions.chmod(0o600)
        _run_openssl(
            [
                "ecparam",
                "-name",
                "prime256v1",
                "-genkey",
                "-noout",
                "-out",
                str(private_key),
            ]
        )
        _run_openssl(
            [
                "req",
                "-new",
                "-sha256",
                "-key",
                str(private_key),
                "-out",
                str(request),
                "-config",
                str(extensions),
            ]
        )
        _run_openssl(
            [
                "x509",
                "-req",
                "-sha256",
                "-in",
                str(request),
                "-CA",
                str(trust.local_ca_certificate),
                "-CAkey",
                str(trust.local_ca_private_key),
                "-set_serial",
                "0x" + secrets.token_hex(16),
                "-days",
                "1",
                "-extfile",
                str(extensions),
                "-extensions",
                "server",
                "-out",
                str(certificate),
            ]
        )
        certificate.chmod(0o600)
        request.unlink(missing_ok=True)
        extensions.unlink(missing_ok=True)
        return EphemeralTlsMaterial(
            session_dir=session,
            private_key=private_key,
            certificate=certificate,
        )
    except BaseException:
        shutil.rmtree(session, ignore_errors=True)
        raise


@dataclass(frozen=True)
class OtaHttpResponse:
    status: int
    headers: Dict[str, str]
    body: bytes
    completes_offer: bool = False


class OtaRequestRouter:
    def __init__(
        self,
        release: OtaRelease,
        *,
        token: str,
        expected_firmware_url: str,
    ) -> None:
        if not _TOKEN.fullmatch(token):
            raise ValueError("OTA URL token must be 24-127 URL-safe characters")
        generation = Path(release.generation_dir).resolve()
        resources = {
            "manifest.json": Path(release.manifest),
            "manifest.sig": Path(release.signature),
            "firmware.bin": Path(release.firmware),
        }
        contents: Dict[str, bytes] = {}
        for expected_name, path in resources.items():
            if path.name != expected_name or path.is_symlink() or not path.is_file():
                raise ValueError("OTA server requires regular immutable generation files")
            if path.resolve().parent != generation:
                raise ValueError("OTA server requires files from the immutable generation")
            contents[expected_name] = path.read_bytes()
        try:
            manifest = json.loads(contents["manifest.json"].decode("utf-8"))
            signed_firmware_url = manifest["artifact"]["url"]
        except (KeyError, TypeError, ValueError, UnicodeDecodeError) as exc:
            raise ValueError("OTA signed manifest artifact URL is missing or invalid") from exc
        if signed_firmware_url != expected_firmware_url:
            raise ValueError(
                "OTA signed manifest artifact URL does not match the reserved endpoint"
            )
        self._token = token
        self._contents = contents

    @property
    def token(self) -> str:
        return self._token

    def route(self, method: str, path: str, headers: Mapping[str, str]) -> OtaHttpResponse:
        normalized_headers = {name.lower(): value for name, value in headers.items()}
        if "transfer-encoding" in normalized_headers:
            return self._error(400)
        if "content-length" in normalized_headers:
            try:
                content_length = int(normalized_headers["content-length"])
            except ValueError:
                return self._error(400)
            if content_length != 0:
                return self._error(400)
        if method not in {"GET", "HEAD"}:
            return self._error(405, {"Allow": "GET, HEAD"})
        if "range" in normalized_headers:
            return self._error(416)

        prefix = f"/{self._token}/"
        if not path.startswith(prefix):
            return self._error(404)
        name = path[len(prefix) :]
        if name not in self._contents or path != prefix + name:
            return self._error(404)
        content = self._contents[name]
        content_type = {
            "manifest.json": "application/json",
            "manifest.sig": "application/octet-stream",
            "firmware.bin": "application/octet-stream",
        }[name]
        response_headers = self._base_headers(len(content))
        response_headers["Content-Type"] = content_type
        return OtaHttpResponse(
            status=200,
            headers=response_headers,
            body=content if method == "GET" else b"",
            completes_offer=method == "GET" and name == "firmware.bin",
        )

    @staticmethod
    def _base_headers(content_length: int) -> Dict[str, str]:
        return {
            "Content-Length": str(content_length),
            "Connection": "close",
            "Cache-Control": "no-store",
            "X-Content-Type-Options": "nosniff",
        }

    @classmethod
    def _error(
        cls,
        status: int,
        extra_headers: Optional[Mapping[str, str]] = None,
    ) -> OtaHttpResponse:
        body = (_STATUS_PHRASES[status] + "\n").encode("ascii")
        headers = cls._base_headers(len(body))
        headers["Content-Type"] = "text/plain; charset=us-ascii"
        if extra_headers:
            headers.update(extra_headers)
        return OtaHttpResponse(status=status, headers=headers, body=body)


@dataclass(frozen=True)
class OtaServerOffer:
    host: str
    port: int
    token: str

    @property
    def base_url(self) -> str:
        return f"https://{self.host}:{self.port}/{self.token}"

    @property
    def manifest_url(self) -> str:
        return self.base_url + "/manifest.json"

    @property
    def signature_url(self) -> str:
        return self.base_url + "/manifest.sig"

    @property
    def firmware_url(self) -> str:
        return self.base_url + "/firmware.bin"


class OtaEndpointReservation:
    def __init__(
        self,
        *,
        offer: OtaServerOffer,
        listening_socket: socket.socket,
    ) -> None:
        self.offer = offer
        self._socket: Optional[socket.socket] = listening_socket
        self._claimed = False

    @classmethod
    def create(
        cls,
        *,
        address_discovery: Callable[[], Iterable[str]] = _discover_active_ipv4,
        token_factory: Callable[[], str] = lambda: secrets.token_urlsafe(32),
        port: int = 0,
    ) -> "OtaEndpointReservation":
        if port < 0 or port > 65535:
            raise ValueError("OTA HTTPS port is out of range")
        host = select_private_lan_ipv4(discovery=address_discovery)
        token = token_factory()
        if not _TOKEN.fullmatch(token):
            raise ValueError("OTA URL token must be 24-127 URL-safe characters")
        listening_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listening_socket.bind((host, port))
            listening_socket.listen(8)
            listening_socket.setblocking(False)
            bound_port = int(listening_socket.getsockname()[1])
            return cls(
                offer=OtaServerOffer(host=host, port=bound_port, token=token),
                listening_socket=listening_socket,
            )
        except BaseException:
            listening_socket.close()
            raise

    @property
    def closed(self) -> bool:
        return self._socket is None

    def claim_socket(self) -> socket.socket:
        if self._claimed or self._socket is None:
            raise RuntimeError("OTA endpoint reservation has already been claimed or closed")
        self._claimed = True
        listening_socket = self._socket
        self._socket = None
        return listening_socket

    def close(self) -> None:
        listening_socket = self._socket
        self._socket = None
        if listening_socket is not None:
            listening_socket.close()


class OtaHttpsServer:
    def __init__(
        self,
        *,
        release: Optional[OtaRelease] = None,
        release_factory: Optional[Callable[[str], OtaRelease]] = None,
        reservation: Optional[OtaEndpointReservation] = None,
        trust: OtaTrustPaths,
        address_discovery: Callable[[], Iterable[str]] = _discover_active_ipv4,
        token_factory: Callable[[], str] = lambda: secrets.token_urlsafe(32),
        session_root: Optional[Path] = None,
        port: int = 0,
        request_timeout: float = 5.0,
        minimum_transfer_bytes_per_second: float = 16 * 1024,
        max_transfer_timeout: float = 300.0,
        max_header_bytes: int = 8192,
        max_headers: int = 48,
        max_concurrency: int = 4,
    ) -> None:
        if (
            not math.isfinite(request_timeout)
            or not math.isfinite(minimum_transfer_bytes_per_second)
            or not math.isfinite(max_transfer_timeout)
            or request_timeout <= 0
            or minimum_transfer_bytes_per_second <= 0
            or max_transfer_timeout <= 0
            or max_header_bytes < 512
            or max_headers < 1
            or max_concurrency < 1
        ):
            raise ValueError("OTA HTTPS resource limits must be positive")
        if (release is None) == (release_factory is None):
            raise ValueError("provide exactly one of release or release_factory")
        if release is not None and reservation is None:
            raise ValueError("a prebuilt OTA release requires a fixed endpoint reservation")
        if reservation is None:
            reservation = OtaEndpointReservation.create(
                address_discovery=address_discovery,
                token_factory=token_factory,
                port=port,
            )
        try:
            if release_factory is not None:
                release = release_factory(reservation.offer.firmware_url)
            if release is None:
                raise ValueError("OTA release factory did not return a release")
            self._router = OtaRequestRouter(
                release,
                token=reservation.offer.token,
                expected_firmware_url=reservation.offer.firmware_url,
            )
        except BaseException:
            reservation.close()
            raise
        self.release = release
        self._reservation = reservation
        self._trust = trust
        self._session_root = session_root
        self._request_timeout = request_timeout
        self._minimum_transfer_bytes_per_second = minimum_transfer_bytes_per_second
        self._max_transfer_timeout = max_transfer_timeout
        self._max_header_bytes = max_header_bytes
        self._max_headers = max_headers
        self._max_concurrency = max_concurrency
        self._server: Optional[asyncio.AbstractServer] = None
        self._material: Optional[EphemeralTlsMaterial] = None
        self._complete = asyncio.Event()
        self._close_lock = asyncio.Lock()
        self._clients: Dict[asyncio.Task, asyncio.StreamWriter] = {}
        self._started = False
        self.session_dir: Optional[Path] = None

    @property
    def offer(self) -> OtaServerOffer:
        return self._reservation.offer

    @property
    def active_connection_count(self) -> int:
        return len(self._clients)

    async def start(self) -> OtaServerOffer:
        if self._started:
            raise RuntimeError("OTA HTTPS server has already been started")
        self._started = True
        host = self.offer.host
        listening_socket: Optional[socket.socket] = None
        try:
            material = create_ephemeral_tls_material(
                trust=self._trust,
                ip_address=host,
                session_root=self._session_root,
            )
            self._material = material
            self.session_dir = material.session_dir
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(
                certfile=str(material.certificate),
                keyfile=str(material.private_key),
            )
            listening_socket = self._reservation.claim_socket()
            self._server = await asyncio.start_server(
                self._handle_connection,
                sock=listening_socket,
                ssl=context,
                limit=self._max_header_bytes + 1,
                ssl_handshake_timeout=self._request_timeout,
            )
            sockets = self._server.sockets or ()
            if len(sockets) != 1:
                raise RuntimeError("OTA HTTPS server did not bind exactly one LAN socket")
            if int(sockets[0].getsockname()[1]) != self.offer.port:
                raise RuntimeError("OTA HTTPS server did not retain the reserved port")
            listening_socket = None
            return self.offer
        except BaseException:
            if listening_socket is not None:
                listening_socket.close()
            await self.close()
            raise

    async def wait_until_complete(self, *, timeout: float) -> None:
        try:
            await asyncio.wait_for(self._complete.wait(), timeout=timeout)
        finally:
            await self.close()

    async def close(self) -> None:
        async with self._close_lock:
            server = self._server
            self._server = None
            if server is not None:
                server.close()
                await server.wait_closed()
            self._reservation.close()
            current_task = asyncio.current_task()
            clients = [
                (task, writer)
                for task, writer in self._clients.items()
                if task is not current_task
            ]
            for _, writer in clients:
                writer.close()
            for task, _ in clients:
                task.cancel()
            if clients:
                await asyncio.gather(
                    *(task for task, _ in clients),
                    return_exceptions=True,
                )
                with contextlib.suppress(Exception):
                    await asyncio.wait_for(
                        asyncio.gather(
                            *(writer.wait_closed() for _, writer in clients),
                            return_exceptions=True,
                        ),
                        timeout=self._request_timeout,
                    )
            material = self._material
            self._material = None
            if material is not None:
                material.cleanup()

    async def __aenter__(self) -> "OtaHttpsServer":
        await self.start()
        return self

    async def __aexit__(self, exc_type, exc, traceback) -> None:
        await self.close()

    async def _handle_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        task = asyncio.current_task()
        if task is None:
            writer.close()
            return
        over_capacity = len(self._clients) >= self._max_concurrency
        self._clients[task] = writer
        loop = asyncio.get_running_loop()
        transfer_deadline: Optional[float] = None
        was_cancelled = False
        try:
            try:
                response = await asyncio.wait_for(
                    self._route_request(reader, over_capacity=over_capacity),
                    timeout=self._request_timeout,
                )
            except asyncio.TimeoutError:
                return
            except asyncio.CancelledError:
                was_cancelled = True
                raise
            except Exception:
                response = OtaRequestRouter._error(500)

            transfer_timeout = self._transfer_timeout(response)
            transfer_deadline = loop.time() + transfer_timeout
            try:
                await asyncio.wait_for(
                    self._write_response(writer, response),
                    timeout=transfer_timeout,
                )
            except asyncio.TimeoutError:
                return
            if response.completes_offer:
                self._complete.set()
                if self._server is not None:
                    self._server.close()
        except asyncio.CancelledError:
            was_cancelled = True
            raise
        except Exception:
            pass
        finally:
            writer.close()
            remaining = (
                transfer_deadline - loop.time()
                if transfer_deadline is not None
                else 0.0
            )
            if not was_cancelled and remaining > 0:
                with contextlib.suppress(BaseException):
                    await asyncio.wait_for(
                        writer.wait_closed(),
                        timeout=remaining,
                    )
            self._clients.pop(task, None)

    async def _route_request(
        self,
        reader: asyncio.StreamReader,
        *,
        over_capacity: bool,
    ) -> OtaHttpResponse:
        if over_capacity:
            return OtaRequestRouter._error(503)
        try:
            raw = await reader.readuntil(b"\r\n\r\n")
        except asyncio.LimitOverrunError:
            return OtaRequestRouter._error(431)
        except (asyncio.IncompleteReadError, ValueError):
            return OtaRequestRouter._error(400)
        if len(raw) > self._max_header_bytes:
            return OtaRequestRouter._error(431)
        try:
            method, target, headers = self._parse_request(raw)
        except ValueError:
            return OtaRequestRouter._error(400)
        return self._router.route(method, target, headers)

    def _transfer_timeout(self, response: OtaHttpResponse) -> float:
        estimated = (
            len(response.body) / self._minimum_transfer_bytes_per_second
            + self._request_timeout
        )
        return min(
            self._max_transfer_timeout,
            max(self._request_timeout, estimated),
        )

    def _parse_request(self, raw: bytes) -> tuple[str, str, Dict[str, str]]:
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("HTTP request headers must be ASCII") from exc
        lines = text[:-4].split("\r\n")
        if not lines or len(lines) - 1 > self._max_headers:
            raise ValueError("too many HTTP headers")
        request_parts = lines[0].split(" ")
        if len(request_parts) != 3 or request_parts[2] not in {"HTTP/1.0", "HTTP/1.1"}:
            raise ValueError("malformed HTTP request line")
        method, target, _ = request_parts
        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if not line or ":" not in line:
                raise ValueError("malformed HTTP header")
            name, value = line.split(":", 1)
            normalized = name.lower()
            if not _HEADER_NAME.fullmatch(name) or normalized in headers:
                raise ValueError("invalid or duplicate HTTP header")
            headers[normalized] = value.strip()
        return method, target, headers

    @staticmethod
    async def _write_response(
        writer: asyncio.StreamWriter, response: OtaHttpResponse
    ) -> None:
        phrase = _STATUS_PHRASES[response.status]
        head = [f"HTTP/1.1 {response.status} {phrase}\r\n"]
        head.extend(f"{name}: {value}\r\n" for name, value in response.headers.items())
        head.append("\r\n")
        writer.write("".join(head).encode("ascii") + response.body)
        await writer.drain()
