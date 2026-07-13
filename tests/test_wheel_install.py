from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


@pytest.mark.skipif(sys.platform != "darwin", reason="native helper is macOS-only")
def test_fresh_wheel_install_builds_and_codesigns_native_helper(tmp_path):
    project = Path(__file__).parents[1]
    repository = tmp_path / "source"
    repository.mkdir()
    shutil.copytree(project / "src", repository / "src")
    for name in ("pyproject.toml", "README.md"):
        shutil.copyfile(project / name, repository / name)
    wheelhouse = tmp_path / "wheelhouse"
    installed = tmp_path / "site"
    home = tmp_path / "home"
    wheelhouse.mkdir()
    installed.mkdir()
    home.mkdir()
    subprocess.run(
        [sys.executable, "-m", "pip", "wheel", ".", "--no-deps", "-w", str(wheelhouse)],
        cwd=repository,
        check=True,
        capture_output=True,
        text=True,
    )
    wheel = next(wheelhouse.glob("code_buddy-*.whl"))
    subprocess.run(
        [sys.executable, "-m", "pip", "install", "--no-deps", "--target", str(installed), str(wheel)],
        check=True,
        capture_output=True,
        text=True,
    )
    environment = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    environment.update(HOME=str(home), PYTHONPATH=str(installed))
    destination = home / ".code-buddy" / "helper" / "CodeBuddyBLEHelper.app"
    subprocess.run(
        [
            sys.executable,
            "-c",
            "from codex_buddy.setup_flow import ensure_helper_app_installed; "
            "print(ensure_helper_app_installed())",
        ],
        cwd=home,
        env=environment,
        check=True,
        capture_output=True,
        text=True,
    )

    executable = destination / "Contents" / "MacOS" / "CodeBuddyBLEHelper"
    assert executable.is_file() and os.access(executable, os.X_OK)
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(destination)],
        check=True,
        capture_output=True,
        text=True,
    )
