#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPOSITORY_ROOT / "src"))

from codex_buddy.ota_trust import export_public_trust, generate_ota_trust


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate protected Code Buddy OTA trust material")
    parser.add_argument("--root", type=Path, help="override the default ~/.code-buddy/ota root")
    parser.add_argument(
        "--export-public",
        type=Path,
        help="export only the CA certificate and manifest verification key",
    )
    arguments = parser.parse_args()

    trust = generate_ota_trust(arguments.root)
    print(f"OTA trust ready under {trust.root}")
    print(f"Local CA certificate: {trust.local_ca_certificate}")
    print(f"Manifest public key: {trust.manifest_public_key}")
    if arguments.export_public:
        exported = export_public_trust(trust, arguments.export_public)
        print(f"Exported public firmware trust to {arguments.export_public}")
        for path in exported:
            print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
