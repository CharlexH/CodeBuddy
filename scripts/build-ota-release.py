#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "src"))

from codex_buddy import runtime
from codex_buddy.ota_release import build_ota_release


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a signed Code Buddy OTA release bundle")
    parser.add_argument("firmware", type=Path, help="application firmware.bin to sign")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--current-version", required=True)
    parser.add_argument("--url", required=True, help="one-time HTTPS firmware URL")
    parser.add_argument("--chip", default="esp32s3")
    parser.add_argument(
        "--signing-key",
        type=Path,
        default=runtime.ota_private_dir() / "manifest-signing-key.pem",
    )
    parser.add_argument(
        "--signing-public-key",
        type=Path,
        default=runtime.ota_public_dir() / "manifest-signing-public.pem",
        help="public key embedded in the target firmware",
    )
    arguments = parser.parse_args()

    release = build_ota_release(
        image_path=arguments.firmware,
        output_dir=arguments.output,
        version=arguments.version,
        current_version=arguments.current_version,
        chip=arguments.chip,
        artifact_url=arguments.url,
        signing_private_key=arguments.signing_key,
        expected_signing_public_key=arguments.signing_public_key,
    )
    print(f"Signed OTA release: {release.output_dir}")
    print(release.firmware)
    print(release.manifest)
    print(release.signature)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
