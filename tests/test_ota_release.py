from __future__ import annotations

import hashlib
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
from codex_buddy.ota_trust import generate_ota_trust


ONE_TIME_URL = "https://192.168.1.20:49321/code-buddy/ota/0123456789abcdef/firmware.bin"


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


def test_release_bundle_binds_firmware_digest_size_and_has_no_private_material(tmp_path):
    trust = generate_ota_trust(tmp_path / "ota")
    image = tmp_path / "source" / "firmware input.bin"
    image.parent.mkdir()
    image.write_bytes(b"firmware-payload\x00\xff")

    release = build_ota_release(
        image_path=image,
        output_dir=tmp_path / "release",
        version="1.2.3",
        current_version="1.2.2",
        chip="esp32s3",
        artifact_url=ONE_TIME_URL,
        signing_private_key=trust.manifest_private_key,
    )

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
    image.write_bytes(b"firmware")

    with pytest.raises(ValueError, match="private"):
        build_ota_release(
            image_path=image,
            output_dir=trust.private_dir / "release",
            version="1.2.3",
            current_version="1.2.2",
            chip="esp32s3",
            artifact_url=ONE_TIME_URL,
            signing_private_key=trust.manifest_private_key,
        )
