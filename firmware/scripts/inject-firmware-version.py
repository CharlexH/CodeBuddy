#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import os
import re
import struct
import tempfile
from pathlib import Path


_SEMVER = re.compile(
    r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)"
    r"(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


def _segments(contents: bytearray):
    if len(contents) < 33 or contents[0] != 0xE9 or contents[12:14] != b"\x09\x00":
        raise ValueError("firmware must be an ESP32-S3 application image")
    count = contents[1]
    if not 1 <= count <= 16:
        raise ValueError("firmware segment count is invalid")
    offset = 24
    segments = []
    for _ in range(count):
        if offset + 8 > len(contents):
            raise ValueError("firmware segment header is truncated")
        _, length = struct.unpack_from("<II", contents, offset)
        offset += 8
        if not length or offset + length > len(contents):
            raise ValueError("firmware segment is truncated")
        segments.append((offset, length))
        offset += length
    return segments, offset


def patch_image_version(path: Path, version: str) -> None:
    if not isinstance(version, str) or not _SEMVER.fullmatch(version):
        raise ValueError("firmware version must be strict semantic version text")
    encoded = version.encode("ascii")
    if len(encoded) >= 32:
        raise ValueError("firmware version does not fit esp_app_desc_t.version")
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("firmware image must be a regular non-symlink file")
    contents = bytearray(path.read_bytes())
    segments, segments_end = _segments(contents)
    first_offset, first_length = segments[0]
    if first_length < 48 or struct.unpack_from("<I", contents, first_offset)[0] != 0xABCD5432:
        raise ValueError("firmware application descriptor is missing")
    version_offset = first_offset + 16
    contents[version_offset : version_offset + 32] = encoded.ljust(32, b"\x00")

    checksum = 0xEF
    for offset, length in segments:
        for value in contents[offset : offset + length]:
            checksum ^= value
    hash_offset = ((segments_end + 16) & ~15)
    checksum_offset = hash_offset - 1
    if contents[23] != 1 or len(contents) != hash_offset + 32:
        raise ValueError("firmware image must carry an appended SHA-256 digest")
    contents[checksum_offset] = checksum
    contents[hash_offset:] = hashlib.sha256(contents[:hash_offset]).digest()

    descriptor, temporary_name = tempfile.mkstemp(prefix=".firmware-version-", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        os.write(descriptor, contents)
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        temporary.chmod(path.stat().st_mode & 0o777)
        os.replace(temporary, path)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        temporary.unlink(missing_ok=True)


def _version_from_header(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(r'^#define CODE_BUDDY_FIRMWARE_VERSION "([^"]+)"$', text, re.MULTILINE)
    if match is None:
        raise ValueError("CODE_BUDDY_FIRMWARE_VERSION is missing")
    return match.group(1)


def _platformio_action(source, target, env) -> None:
    project = Path(env.subst("$PROJECT_DIR"))
    image = Path(env.subst("$BUILD_DIR")) / (env.subst("$PROGNAME") + ".bin")
    patch_image_version(
        image, _version_from_header(project / "src" / "firmware_version.h")
    )


try:
    Import("env")  # type: ignore[name-defined]  # noqa: F821
except NameError:
    env = None

if env is not None:
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _platformio_action)
