from __future__ import annotations

import errno
import fcntl
import os
import shutil
import stat
from pathlib import Path
from typing import Iterable, Optional


class AgentProcessLock:
    """A process-scoped, non-blocking lock for the one local buddy agent."""

    def __init__(self, path: Path) -> None:
        self.path = Path(path)
        self._descriptor: Optional[int] = None

    @property
    def held(self) -> bool:
        return self._descriptor is not None

    def acquire(self) -> None:
        if self._descriptor is not None:
            raise RuntimeError("buddy agent lock is already held by this instance")
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.path.parent.is_symlink() or not self.path.parent.is_dir():
            raise RuntimeError("buddy agent lock parent must be a real directory")
        flags = os.O_CREAT | os.O_RDWR
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        try:
            descriptor = os.open(self.path, flags, 0o600)
        except OSError as exc:
            raise RuntimeError("cannot safely open buddy agent lock") from exc
        try:
            metadata = os.fstat(descriptor)
            if not stat.S_ISREG(metadata.st_mode):
                raise RuntimeError("buddy agent lock must be a regular file")
            os.fchmod(descriptor, 0o600)
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                if exc.errno in (errno.EACCES, errno.EAGAIN):
                    raise RuntimeError("buddy agent is already running") from exc
                raise RuntimeError("cannot acquire buddy agent lock") from exc
        except BaseException:
            os.close(descriptor)
            raise
        self._descriptor = descriptor

    def release(self) -> None:
        descriptor = self._descriptor
        if descriptor is None:
            return
        self._descriptor = None
        try:
            fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _real_directory_or_missing(path: Path) -> bool:
    if not os.path.lexists(path):
        return False
    if path.is_symlink():
        raise ValueError(f"managed OTA root must not be a symlink: {path}")
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ValueError(f"cannot inspect managed OTA root: {path}") from exc
    if not stat.S_ISDIR(metadata.st_mode):
        raise ValueError(f"managed OTA root must be a directory: {path}")
    return True


def _require_real_directory(path: Path, *, description: str) -> None:
    if path.is_symlink():
        raise ValueError(f"{description} must not be a symlink")
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise ValueError(f"cannot inspect {description}") from exc
    if not stat.S_ISDIR(metadata.st_mode):
        raise ValueError(f"{description} must be a real directory")


def _require_regular_files(directory: Path, allowed: Iterable[str], *, exact: bool) -> None:
    allowed_names = set(allowed)
    children = list(directory.iterdir())
    names = {child.name for child in children}
    if (exact and names != allowed_names) or (not exact and not names <= allowed_names):
        raise ValueError(f"unexpected node in managed OTA residue: {directory}")
    for child in children:
        if child.is_symlink():
            raise ValueError(f"managed OTA residue child must not be a symlink: {child}")
        try:
            metadata = child.lstat()
        except OSError as exc:
            raise ValueError(f"cannot inspect managed OTA residue: {child}") from exc
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f"managed OTA residue child must be a regular file: {child}")


def _require_release_staging_files(directory: Path) -> None:
    canonical = {
        "firmware.bin",
        "manifest.json",
        "manifest.sig",
        ".verification-public.pem",
    }
    for child in directory.iterdir():
        is_atomic_temporary = any(
            child.name.startswith(f".{name}.") and len(child.name) > len(name) + 2
            for name in canonical
        )
        if child.name not in canonical and not is_atomic_temporary:
            raise ValueError(f"unexpected node in managed OTA residue: {directory}")
        if child.is_symlink() or not stat.S_ISREG(child.lstat().st_mode):
            raise ValueError(f"managed OTA residue child must be a regular file: {child}")


def _contained_symlink(path: Path, root: Path) -> None:
    if not path.is_symlink():
        raise ValueError(f"managed OTA pointer must be a symlink: {path}")
    target = Path(os.readlink(path))
    if target.is_absolute():
        raise ValueError("managed OTA pointer symlink must be relative")
    resolved_root = root.resolve()
    resolved_target = (path.parent / target).resolve(strict=False)
    try:
        resolved_target.relative_to(resolved_root)
    except ValueError as exc:
        raise ValueError("managed OTA pointer symlink escapes its controlled root") from exc


def _snapshot_cleanup_targets(root: Path) -> list[Path]:
    if not _real_directory_or_missing(root):
        return []
    targets: list[Path] = []
    for child in root.iterdir():
        if not child.name.startswith(".snapshot-"):
            raise ValueError(f"unexpected node in managed OTA snapshot root: {child}")
        _require_real_directory(child, description="OTA snapshot directory")
        _require_regular_files(child, {"firmware.bin"}, exact=True)
        targets.append(child)
    return targets


def _session_cleanup_targets(root: Path) -> list[Path]:
    if not _real_directory_or_missing(root):
        return []
    targets: list[Path] = []
    allowed = {"leaf-key.pem", "leaf-cert.pem", "leaf.csr", "leaf-extensions.cnf"}
    for child in root.iterdir():
        if not child.name.startswith("https-"):
            raise ValueError(f"unexpected node in managed OTA session root: {child}")
        _require_real_directory(child, description="OTA TLS session directory")
        _require_regular_files(child, allowed, exact=False)
        targets.append(child)
    return targets


def _release_cleanup_targets(root: Path) -> tuple[list[Path], list[Path]]:
    if not _real_directory_or_missing(root):
        return [], []
    generation_root = root / ".current.generations"
    directories: list[Path] = []
    pointers: list[Path] = []
    for child in root.iterdir():
        if child.name == ".current.generations":
            _require_real_directory(child, description="OTA release generation root")
        elif child.name == ".current.build.lock":
            if child.is_symlink() or not stat.S_ISREG(child.lstat().st_mode):
                raise ValueError("OTA release build lock must be a regular file")
        elif child.name == "current":
            _contained_symlink(child, generation_root)
        elif child.name.startswith(".current.current-"):
            _contained_symlink(child, generation_root)
            pointers.append(child)
        else:
            raise ValueError(f"unexpected node in managed OTA release root: {child}")

    if generation_root.exists():
        for generation in generation_root.iterdir():
            _require_real_directory(generation, description="OTA release generation")
            if generation.name.startswith(".staging-"):
                _require_release_staging_files(generation)
                directories.append(generation)
            else:
                _require_regular_files(
                    generation,
                    {"firmware.bin", "manifest.json", "manifest.sig"},
                    exact=True,
                )
    return directories, pointers


def cleanup_stale_ota_runtime(
    *, snapshots_root: Path, sessions_root: Path, releases_root: Path
) -> None:
    """Remove only structurally recognized crash residue from controlled OTA roots."""

    snapshot_targets = _snapshot_cleanup_targets(Path(snapshots_root))
    session_targets = _session_cleanup_targets(Path(sessions_root))
    release_directories, release_pointers = _release_cleanup_targets(Path(releases_root))
    # Validation is deliberately completed before the first deletion.
    for directory in snapshot_targets + session_targets + release_directories:
        shutil.rmtree(directory)
    for pointer in release_pointers:
        pointer.unlink()
