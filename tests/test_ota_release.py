from __future__ import annotations

import hashlib
import os
import struct
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

from codex_buddy.ota_release import (
    build_ota_release,
    canonical_manifest_bytes,
    compare_semantic_versions,
    require_monotonic_version,
    sign_manifest,
    verify_manifest_signature,
)
from codex_buddy.ota_trust import bootstrap_ota_trust, generate_ota_trust


ONE_TIME_URL = "https://192.168.1.20:49321/0123456789abcdefghijklmn/firmware.bin"


def _certificate_der_sha256(path: Path) -> str:
    der = subprocess.run(
        ["openssl", "x509", "-in", str(path), "-outform", "DER"],
        check=True,
        capture_output=True,
    ).stdout
    return hashlib.sha256(der).hexdigest()


def _public_key_der_sha256(path: Path) -> str:
    der = subprocess.run(
        ["openssl", "pkey", "-pubin", "-in", str(path), "-outform", "DER"],
        check=True,
        capture_output=True,
    ).stdout
    return hashlib.sha256(der).hexdigest()


def _esp32s3_image(payload: bytes = b"firmware-payload") -> bytes:
    header = bytearray(24)
    header[0] = 0xE9
    header[1] = 1
    header[2] = 2
    header[3] = 0x3F
    header[4:8] = struct.pack("<I", 0x40377B4C)
    header[8] = 0xEE
    header[12:14] = struct.pack("<H", 9)
    header[23] = 0
    return bytes(header) + struct.pack("<II", 0x3C0E0020, len(payload)) + payload + b"\xef"


def test_manifest_has_exact_deterministic_canonical_bytes():
    digest = "ab" * 32

    manifest = canonical_manifest_bytes(
        version="1.2.3",
        chip="esp32s3",
        size_bytes=1234,
        sha256=digest,
        artifact_url=ONE_TIME_URL,
    )

    assert manifest == (
        b'{"artifact":{"sha256":"'
        + digest.encode()
        + b'","sizeBytes":1234,"url":"'
        + ONE_TIME_URL.encode()
        + b'"},"chip":"esp32s3","schema":1,"version":"1.2.3"}'
    )
    assert manifest == canonical_manifest_bytes(
        artifact_url=ONE_TIME_URL,
        sha256=digest,
        size_bytes=1234,
        chip="esp32s3",
        version="1.2.3",
    )
    assert not manifest.endswith(b"\n")


@pytest.mark.parametrize(
    ("kwargs", "message"),
    [
        ({"artifact_url": "http://192.168.1.20/firmware.bin"}, "HTTPS"),
        ({"artifact_url": "https://user@192.168.1.20/firmware.bin"}, "credentials"),
        ({"artifact_url": ONE_TIME_URL + "#fragment"}, "fragment"),
        ({"sha256": "AB" * 32}, "SHA-256"),
        ({"sha256": "ab" * 31}, "SHA-256"),
        ({"size_bytes": 0}, "size"),
        ({"size_bytes": 0x330001}, "slot"),
        ({"chip": "esp32"}, "chip"),
    ],
)
def test_manifest_rejects_values_that_cannot_be_bound(kwargs, message):
    values = {
        "version": "1.2.3",
        "chip": "esp32s3",
        "size_bytes": 1,
        "sha256": "ab" * 32,
        "artifact_url": ONE_TIME_URL,
    }
    values.update(kwargs)

    with pytest.raises(ValueError, match=message):
        canonical_manifest_bytes(**values)


def test_p256_signature_verifies_exact_manifest_bytes_offline(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    manifest = canonical_manifest_bytes(
        version="1.2.3",
        chip="esp32s3",
        size_bytes=4,
        sha256=hashlib.sha256(b"test").hexdigest(),
        artifact_url=ONE_TIME_URL,
    )

    signature = sign_manifest(manifest, trust.manifest_private_key)

    assert verify_manifest_signature(manifest, signature, trust.manifest_public_key)
    assert not verify_manifest_signature(manifest + b" ", signature, trust.manifest_public_key)


def test_semantic_version_policy_is_strict_and_monotonic():
    assert compare_semantic_versions("1.0.0", "0.9.9") > 0
    assert compare_semantic_versions("1.0.0-rc.1", "1.0.0-rc.2") < 0
    assert compare_semantic_versions("1.0.0", "1.0.0-rc.2") > 0
    assert require_monotonic_version("2.0.0", "1.9.9") == "2.0.0"

    for candidate in ("1.9.9", "2.0.0"):
        with pytest.raises(ValueError, match="newer"):
            require_monotonic_version(candidate, "2.0.0")
    for invalid in ("1.2", "01.2.3", "1.2.3.4", "v1.2.3"):
        with pytest.raises(ValueError, match="semantic version"):
            compare_semantic_versions(invalid, "1.0.0")


def test_host_semver_and_byte_bounds_match_firmware_parser():
    max_version = "1.2.3-" + "a" * 57
    assert len(max_version.encode("utf-8")) == 63
    assert compare_semantic_versions(max_version, "1.2.2") > 0

    for invalid in (
        "١.2.3",
        "4294967296.0.0",
        "1.4294967296.0",
        "1.2.4294967296",
        "1.2.3-" + "a" * 58,
    ):
        with pytest.raises(ValueError, match="semantic version"):
            compare_semantic_versions(invalid, "1.0.0")


def test_host_accepts_device_maximum_canonical_boundaries():
    version = "1.2.3-" + "a" * 57
    token = "t" * 127
    url = f"https://192.168.255.255:65535/{token}/firmware.bin"

    manifest = canonical_manifest_bytes(
        version=version,
        chip="esp32s3",
        size_bytes=0x330000,
        sha256="ab" * 32,
        artifact_url=url,
    )

    assert len(version.encode("utf-8")) == 63
    assert len(token.encode("utf-8")) == 127
    assert len(url.encode("utf-8")) <= 255
    assert len(manifest) <= 1024


def test_host_url_policy_matches_device_one_shot_endpoint():
    invalid_urls = (
        "https://8.8.8.8:49321/0123456789abcdefghijklmn/firmware.bin",
        "https://127.0.0.1:49321/0123456789abcdefghijklmn/firmware.bin",
        "https://169.254.1.1:49321/0123456789abcdefghijklmn/firmware.bin",
        "https://192.168.1.20/0123456789abcdefghijklmn/firmware.bin",
        "https://192.168.1.20:049321/0123456789abcdefghijklmn/firmware.bin",
        "https://192.168.1.20:49321/short-token/firmware.bin",
        "https://192.168.1.20:49321/prefix/0123456789abcdefghijklmn/firmware.bin",
        "https://192.168.1.20:49321/0123456789abcdefghijklmn/%66irmware.bin",
        "https://192.168.1.20:49321/0123456789abcdefghijklmn/firmware.bin?",
        "https://192.168.1.20:49321/0123456789abcdefghijklmn/firmware.bin#",
        f"https://192.168.1.20:49321/{'t' * 128}/firmware.bin",
    )
    for artifact_url in invalid_urls:
        with pytest.raises(ValueError, match="artifact URL"):
            canonical_manifest_bytes(
                version="1.2.3",
                chip="esp32s3",
                size_bytes=1234,
                sha256="ab" * 32,
                artifact_url=artifact_url,
            )


def test_firmware_current_version_matches_host_package_version():
    project = Path(__file__).resolve().parents[1]
    package_version = __import__("codex_buddy").__version__
    version_header = (project / "firmware/src/firmware_version.h").read_text()

    assert f'#define CODE_BUDDY_FIRMWARE_VERSION "{package_version}"' in version_header


def test_public_trust_injection_is_deterministic_and_contains_no_private_key(tmp_path):
    project = Path(__file__).resolve().parents[1]
    script = project / "firmware/scripts/inject-ota-trust.py"
    trust = generate_ota_trust(tmp_path / "managed trust")
    output = tmp_path / "generated" / "ota_trust_generated.h"
    arguments = [
        "python3", str(script),
        "--public-dir", str(trust.public_dir),
        "--output", str(output),
        "--expected-ca-sha256", _certificate_der_sha256(trust.local_ca_certificate),
        "--expected-public-sha256", _public_key_der_sha256(trust.manifest_public_key),
    ]

    subprocess.run(arguments, check=True, capture_output=True)
    first = output.read_bytes()
    subprocess.run(arguments, check=True, capture_output=True)

    assert output.read_bytes() == first
    assert b"BEGIN CERTIFICATE" in first
    assert b"BEGIN PUBLIC KEY" in first
    assert b"PRIVATE KEY" not in first


def test_public_trust_injection_fails_closed_for_missing_malformed_or_mismatched_material(
    tmp_path,
):
    project = Path(__file__).resolve().parents[1]
    script = project / "firmware/scripts/inject-ota-trust.py"
    trust = generate_ota_trust(tmp_path / "trust")
    other = generate_ota_trust(tmp_path / "other")
    output = tmp_path / "ota_trust_generated.h"

    cases = [
        (tmp_path / "missing", "00" * 32, "00" * 32),
        (trust.public_dir, "00" * 32, _public_key_der_sha256(trust.manifest_public_key)),
        (trust.public_dir, _certificate_der_sha256(trust.local_ca_certificate),
         _public_key_der_sha256(other.manifest_public_key)),
    ]
    for public_dir, ca_digest, public_digest in cases:
        output.write_text("stale trust must not survive")
        completed = subprocess.run(
            [
                "python3", str(script), "--public-dir", str(public_dir),
                "--output", str(output), "--expected-ca-sha256", ca_digest,
                "--expected-public-sha256", public_digest,
            ],
            capture_output=True,
        )
        assert completed.returncode != 0
        assert not output.exists()

    trust.manifest_public_key.write_text("not a public key")
    completed = subprocess.run(
        [
            "python3", str(script), "--public-dir", str(trust.public_dir),
            "--output", str(output),
            "--expected-ca-sha256", _certificate_der_sha256(trust.local_ca_certificate),
            "--expected-public-sha256", "00" * 32,
        ],
        capture_output=True,
    )
    assert completed.returncode != 0
    assert not output.exists()


def test_managed_injection_requires_independent_pins_and_never_self_pins(tmp_path):
    project = Path(__file__).resolve().parents[1]
    script = project / "firmware/scripts/inject-ota-trust.py"
    output = tmp_path / "ota_trust_generated.h"
    trust = generate_ota_trust(tmp_path / "managed")
    command = [
        "python3", str(script), "--managed-root", str(trust.root),
        "--output", str(output),
    ]

    missing_pins = subprocess.run(command, capture_output=True)
    assert missing_pins.returncode != 0
    assert not output.exists()
    assert not trust.trust_pins.exists()

    bootstrap_ota_trust(trust.root)
    subprocess.run(command, check=True, capture_output=True)
    assert output.is_file()

    other = generate_ota_trust(tmp_path / "other")
    trust.manifest_public_key.write_bytes(other.manifest_public_key.read_bytes())
    output.write_text("stale header")
    mismatch = subprocess.run(command, capture_output=True)
    assert mismatch.returncode != 0
    assert not output.exists()


def test_explicit_generation_command_adds_pins_without_rotating_existing_keys(tmp_path):
    project = Path(__file__).resolve().parents[1]
    generator = project / "scripts/generate-ota-trust.py"
    trust = generate_ota_trust(tmp_path / "managed")
    original_private = (
        trust.local_ca_private_key.read_bytes(),
        trust.manifest_private_key.read_bytes(),
    )

    subprocess.run(
        ["python3", str(generator), "--root", str(trust.root)],
        check=True,
        capture_output=True,
    )

    assert trust.trust_pins.is_file()
    assert (
        trust.local_ca_private_key.read_bytes(),
        trust.manifest_private_key.read_bytes(),
    ) == original_private


def test_release_bundle_binds_firmware_digest_size_and_has_no_private_material(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "source" / "firmware input.bin"
    image.parent.mkdir()
    image.write_bytes(_esp32s3_image(b"firmware-payload\x00\xff"))

    release = build_ota_release(
        image_path=image,
        output_dir=tmp_path / "release",
        version="1.2.3",
        current_version="1.2.2",
        chip="esp32s3",
        artifact_url=ONE_TIME_URL,
        signing_private_key=trust.manifest_private_key,
        expected_signing_public_key=trust.manifest_public_key,
    )

    assert release.output_dir == tmp_path / "release"
    assert release.output_dir.is_symlink()
    assert {path.name for path in release.output_dir.iterdir()} == {
        "firmware.bin",
        "manifest.json",
        "manifest.sig",
    }
    assert release.firmware.read_bytes() == image.read_bytes()
    expected_manifest = canonical_manifest_bytes(
        version="1.2.3",
        chip="esp32s3",
        size_bytes=len(image.read_bytes()),
        sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
        artifact_url=ONE_TIME_URL,
    )
    assert release.manifest.read_bytes() == expected_manifest
    assert verify_manifest_signature(
        expected_manifest,
        release.signature.read_bytes(),
        trust.manifest_public_key,
    )
    assert not any("PRIVATE KEY" in path.read_text(errors="ignore") for path in release.output_dir.iterdir())


def test_release_refuses_output_inside_private_trust_directory(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image())

    with pytest.raises(ValueError, match="private"):
        build_ota_release(
            image_path=image,
            output_dir=trust.private_dir / "release",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=trust.manifest_private_key,
            expected_signing_public_key=trust.manifest_public_key,
        )


def test_release_rejects_private_key_and_symlink_as_firmware(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image_symlink = tmp_path / "firmware.bin"
    image_symlink.symlink_to(trust.manifest_private_key)

    for image in (trust.manifest_private_key, image_symlink):
        with pytest.raises(ValueError, match="firmware image"):
            build_ota_release(
                image_path=image,
                output_dir=tmp_path / f"release-{image.name}",
                version="1.2.3",
                current_version="1.2.2",
                chip="esp32s3",
                artifact_url=ONE_TIME_URL,
                signing_private_key=trust.manifest_private_key,
                expected_signing_public_key=trust.manifest_public_key,
            )


def test_release_rejects_non_esp32s3_application_image(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    wrong_chip = bytearray(_esp32s3_image())
    wrong_chip[12:14] = struct.pack("<H", 2)

    for number, contents in enumerate((b"not firmware", bytes(wrong_chip), b"\xe9" * 32)):
        image = tmp_path / f"invalid-{number}.bin"
        image.write_bytes(contents)
        with pytest.raises(ValueError, match="ESP32-S3 application image"):
            build_ota_release(
                image_path=image,
                output_dir=tmp_path / f"release-{number}",
                version="1.2.3",
                current_version="1.2.2",
                chip="esp32s3",
                artifact_url=ONE_TIME_URL,
                signing_private_key=trust.manifest_private_key,
                expected_signing_public_key=trust.manifest_public_key,
            )


def test_atomic_writes_handle_short_os_writes(tmp_path, monkeypatch):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image(b"short-write" * 100))
    real_write = os.write

    def short_write(descriptor, contents):
        return real_write(descriptor, contents[: max(1, min(7, len(contents)))])

    monkeypatch.setattr(os, "write", short_write)

    release = build_ota_release(
        image_path=image,
        output_dir=tmp_path / "release",
        version="1.2.3",
        current_version="1.2.2",
        chip="esp32s3",
        artifact_url=ONE_TIME_URL,
        signing_private_key=trust.manifest_private_key,
        expected_signing_public_key=trust.manifest_public_key,
    )

    assert release.firmware.read_bytes() == image.read_bytes()
    assert verify_manifest_signature(
        release.manifest.read_bytes(),
        release.signature.read_bytes(),
        trust.manifest_public_key,
    )


def test_interleaved_builds_publish_immutable_complete_generations(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    images = []
    for number in range(2):
        image = tmp_path / f"firmware-{number}.bin"
        image.write_bytes(_esp32s3_image(bytes([number]) * 4096))
        images.append(image)

    def build(number):
        return build_ota_release(
            image_path=images[number],
            output_dir=tmp_path / "release",
            version=f"1.2.{number + 3}",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL.replace("0123456789abcdef", f"0123456789abcde{number}"),
            signing_private_key=trust.manifest_private_key,
            expected_signing_public_key=trust.manifest_public_key,
        )

    with ThreadPoolExecutor(max_workers=2) as executor:
        releases = list(executor.map(build, range(2)))

    assert releases[0].generation_dir != releases[1].generation_dir
    for release, image in zip(releases, images):
        assert {path.name for path in release.generation_dir.iterdir()} == {
            "firmware.bin",
            "manifest.json",
            "manifest.sig",
        }
        assert release.firmware.read_bytes() == image.read_bytes()
        assert verify_manifest_signature(
            release.manifest.read_bytes(),
            release.signature.read_bytes(),
            trust.manifest_public_key,
        )
    current = (tmp_path / "release").resolve()
    assert current in {release.generation_dir.resolve() for release in releases}


def test_release_rejects_rsa_signing_key(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image())
    rsa_key = tmp_path / "rsa-key.pem"
    subprocess.run(
        ["openssl", "genrsa", "-out", str(rsa_key), "2048"],
        check=True,
        capture_output=True,
    )
    rsa_key.chmod(0o600)

    with pytest.raises(ValueError, match="P-256"):
        build_ota_release(
            image_path=image,
            output_dir=tmp_path / "release",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=rsa_key,
            expected_signing_public_key=trust.manifest_public_key,
        )


def test_release_rejects_p256_key_not_bound_to_expected_public_key(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    other = generate_ota_trust(tmp_path / "other")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image())

    with pytest.raises(ValueError, match="expected firmware public key"):
        build_ota_release(
            image_path=image,
            output_dir=tmp_path / "release",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=other.manifest_private_key,
            expected_signing_public_key=trust.manifest_public_key,
        )


def test_release_rejects_signing_key_symlink_or_permissive_mode(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image())
    signing_symlink = tmp_path / "signing-key.pem"
    signing_symlink.symlink_to(trust.manifest_private_key)

    with pytest.raises(ValueError, match="regular non-symlink"):
        build_ota_release(
            image_path=image,
            output_dir=tmp_path / "release-symlink",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=signing_symlink,
            expected_signing_public_key=trust.manifest_public_key,
        )

    trust.manifest_private_key.chmod(0o640)
    with pytest.raises(ValueError, match="0600"):
        build_ota_release(
            image_path=image,
            output_dir=tmp_path / "release-mode",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=trust.manifest_private_key,
            expected_signing_public_key=trust.manifest_public_key,
        )


def test_release_accepts_protected_managed_p256_pair(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "firmware.bin"
    image.write_bytes(_esp32s3_image())

    release = build_ota_release(
        image_path=image,
        output_dir=tmp_path / "release",
        version="1.2.3",
        current_version="1.2.2",
        chip="esp32s3",
        artifact_url=ONE_TIME_URL,
        signing_private_key=trust.manifest_private_key,
        expected_signing_public_key=trust.manifest_public_key,
    )

    assert verify_manifest_signature(
        release.manifest.read_bytes(),
        release.signature.read_bytes(),
        trust.manifest_public_key,
    )
