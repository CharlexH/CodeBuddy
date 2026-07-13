from __future__ import annotations

import contextlib
import shutil
import subprocess
import tempfile
from importlib import resources
from pathlib import Path


def build_bundled_native_helper() -> Path:
    """Build an ad-hoc signed helper exclusively from packaged resources."""

    build_root = Path(tempfile.mkdtemp(prefix="codebuddy-native-helper-"))
    marker = build_root / ".codebuddy-native-build"
    marker.write_text("managed\n", encoding="ascii")
    app = build_root / "CodeBuddyBLEHelper.app"
    executable = app / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
    plist_destination = app / "Contents" / "Info.plist"
    executable.parent.mkdir(parents=True)
    package = resources.files("codex_buddy").joinpath("native_ble_helper")
    try:
        with contextlib.ExitStack() as stack:
            source = stack.enter_context(
                resources.as_file(package.joinpath("CodeBuddyBLEHelper.swift"))
            )
            plist = stack.enter_context(resources.as_file(package.joinpath("Info.plist")))
            shutil.copyfile(plist, plist_destination)
            subprocess.run(
                [
                    "swiftc", "-parse-as-library", "-O",
                    "-framework", "AppKit", "-framework", "CoreBluetooth",
                    str(source), "-o", str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        subprocess.run(
            ["xattr", "-cr", str(app)], check=True, capture_output=True, text=True
        )
        subprocess.run(
            ["codesign", "--force", "--sign", "-", str(app)],
            check=True,
            capture_output=True,
            text=True,
        )
        subprocess.run(
            ["codesign", "--verify", "--deep", "--strict", str(app)],
            check=True,
            capture_output=True,
            text=True,
        )
        return app
    except BaseException:
        shutil.rmtree(build_root, ignore_errors=True)
        raise


def cleanup_bundled_native_helper_build(app: Path) -> None:
    build_root = Path(app).parent
    if (build_root / ".codebuddy-native-build").is_file():
        shutil.rmtree(build_root, ignore_errors=True)
