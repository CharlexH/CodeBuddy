from __future__ import annotations

import hashlib
import importlib.util
import struct
from pathlib import Path

from codex_buddy.ota_release import inspect_esp32s3_application_image


SCRIPT = Path(__file__).resolve().parents[1] / "firmware" / "scripts" / "inject-firmware-version.py"
spec = importlib.util.spec_from_file_location("inject_firmware_version", SCRIPT)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def _image(version: str) -> bytes:
    descriptor = bytearray(256)
    descriptor[:4] = struct.pack("<I", 0xABCD5432)
    descriptor[16 : 16 + len(version)] = version.encode("ascii")
    header = bytearray(24)
    header[0], header[1], header[2], header[3] = 0xE9, 2, 2, 0x3F
    header[4:8] = struct.pack("<I", 0x40377B44)
    header[12:14] = struct.pack("<H", 9)
    header[23] = 1
    code = bytearray(16)
    raw = header + struct.pack("<II", 0x3C000020, len(descriptor)) + descriptor
    raw += struct.pack("<II", 0x40377B40, len(code)) + code
    checksum = 0xEF
    for value in descriptor:
        checksum ^= value
    for value in code:
        checksum ^= value
    while (len(raw) + 1) % 16:
        raw.append(0)
    raw.append(checksum)
    raw.extend(hashlib.sha256(raw).digest())
    return bytes(raw)


def test_patch_embeds_semver_and_rebuilds_esp_image_checksum_and_hash(tmp_path):
    image = tmp_path / "firmware.bin"
    image.write_bytes(_image("esp-idf: v4.4.7"))

    module.patch_image_version(image, "0.1.4")

    assert inspect_esp32s3_application_image(image).version == "0.1.4"
    contents = image.read_bytes()
    assert hashlib.sha256(contents[:-32]).digest() == contents[-32:]
    first = contents
    module.patch_image_version(image, "0.1.4")
    assert image.read_bytes() == first
