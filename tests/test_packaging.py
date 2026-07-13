from __future__ import annotations

import shutil
import subprocess
import tarfile
import zipfile
from pathlib import Path


RESOURCE = "code-buddy-sticks3-app.bin"


def test_wheel_and_sdist_contain_the_default_firmware_resource(tmp_path):
    project = Path(__file__).resolve().parents[1]
    source_resource = project / "src" / "codex_buddy" / "firmware" / RESOURCE
    assert source_resource.is_file(), "release app image must be staged as package data"

    isolated = tmp_path / "project"
    shutil.copytree(project / "src", isolated / "src")
    shutil.copy2(project / "pyproject.toml", isolated / "pyproject.toml")
    shutil.copy2(project / "README.md", isolated / "README.md")
    output = tmp_path / "dist"
    output.mkdir()
    uv = shutil.which("uv")
    assert uv is not None, "uv is required for the repository packaging integration test"
    subprocess.run(
        [
            uv,
            "build",
            "--wheel",
            "--sdist",
            "--out-dir",
            str(output),
            str(isolated),
        ],
        check=True,
        capture_output=True,
        text=True,
    )

    wheel = next(output.glob("*.whl"))
    sdist = next(output.glob("*.tar.gz"))
    with zipfile.ZipFile(wheel) as archive:
        wheel_members = [name for name in archive.namelist() if name.endswith(RESOURCE)]
        assert wheel_members == [f"codex_buddy/firmware/{RESOURCE}"]
        assert archive.read(wheel_members[0]) == source_resource.read_bytes()
    with tarfile.open(sdist) as archive:
        sdist_members = [name for name in archive.getnames() if name.endswith(RESOURCE)]
        assert len(sdist_members) == 1
        extracted = archive.extractfile(sdist_members[0])
        assert extracted is not None
        assert extracted.read() == source_resource.read_bytes()
