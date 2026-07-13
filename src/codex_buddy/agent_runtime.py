from __future__ import annotations

import errno
import contextlib
import fcntl
import os
import stat
from pathlib import Path
from pathlib import PurePath
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


_DIRECTORY_FLAGS = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0)


def _open_real_directory(path: Path) -> Optional[int]:
    """Open an existing directory without following any path component."""

    absolute = Path(os.path.abspath(path))
    descriptor = os.open(os.path.sep, _DIRECTORY_FLAGS)
    try:
        for component in absolute.parts[1:]:
            try:
                next_descriptor = os.open(
                    component, _DIRECTORY_FLAGS, dir_fd=descriptor
                )
            except FileNotFoundError:
                os.close(descriptor)
                return None
            except OSError as exc:
                raise ValueError(
                    f"managed OTA root ancestor must not be a symlink: {path}"
                ) from exc
            os.close(descriptor)
            descriptor = next_descriptor
        return descriptor
    except BaseException:
        with contextlib.suppress(OSError):
            os.close(descriptor)
        raise


def _metadata(descriptor: int, name: str, *, description: str) -> os.stat_result:
    try:
        return os.stat(name, dir_fd=descriptor, follow_symlinks=False)
    except OSError as exc:
        raise ValueError(f"cannot inspect {description}") from exc


def _open_child_directory(descriptor: int, name: str, *, description: str) -> int:
    try:
        return os.open(name, _DIRECTORY_FLAGS, dir_fd=descriptor)
    except OSError as exc:
        raise ValueError(f"{description} must be a real directory, not a symlink") from exc


def _require_regular_files(
    descriptor: int, allowed: Iterable[str], *, exact: bool
) -> None:
    allowed_names = set(allowed)
    names = set(os.listdir(descriptor))
    if (exact and names != allowed_names) or (not exact and not names <= allowed_names):
        raise ValueError("unexpected node in managed OTA residue")
    for name in names:
        metadata = _metadata(descriptor, name, description="managed OTA residue")
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("managed OTA residue child must be a regular file, not a symlink")


def _require_release_staging_files(descriptor: int) -> None:
    canonical = {
        "firmware.bin",
        "manifest.json",
        "manifest.sig",
        ".verification-public.pem",
    }
    for name in os.listdir(descriptor):
        is_atomic_temporary = any(
            name.startswith(f".{canonical_name}.") and
            len(name) > len(canonical_name) + 2
            for canonical_name in canonical
        )
        if name not in canonical and not is_atomic_temporary:
            raise ValueError("unexpected node in managed OTA residue")
        if not stat.S_ISREG(
            _metadata(descriptor, name, description="managed OTA release residue").st_mode
        ):
            raise ValueError("managed OTA residue child must be a regular file, not a symlink")


def _contained_symlink(descriptor: int, name: str) -> None:
    metadata = _metadata(descriptor, name, description="managed OTA pointer")
    if not stat.S_ISLNK(metadata.st_mode):
        raise ValueError("managed OTA pointer must be a symlink")
    target = PurePath(os.readlink(name, dir_fd=descriptor))
    if target.is_absolute():
        raise ValueError("managed OTA pointer symlink must be relative")
    if not target.parts or target.parts[0] != ".current.generations" or ".." in target.parts:
        raise ValueError("managed OTA pointer symlink escapes its controlled root")


def _snapshot_cleanup_targets(root: Optional[int]) -> list[str]:
    if root is None:
        return []
    targets: list[str] = []
    for name in os.listdir(root):
        if not name.startswith(".snapshot-"):
            raise ValueError("unexpected node in managed OTA snapshot root")
        child = _open_child_directory(root, name, description="OTA snapshot directory")
        try:
            _require_regular_files(child, {"firmware.bin"}, exact=True)
        finally:
            os.close(child)
        targets.append(name)
    return targets


def _session_cleanup_targets(root: Optional[int]) -> list[str]:
    if root is None:
        return []
    targets: list[str] = []
    allowed = {"leaf-key.pem", "leaf-cert.pem", "leaf.csr", "leaf-extensions.cnf"}
    for name in os.listdir(root):
        if not name.startswith("https-"):
            raise ValueError("unexpected node in managed OTA session root")
        child = _open_child_directory(root, name, description="OTA TLS session directory")
        try:
            _require_regular_files(child, allowed, exact=False)
        finally:
            os.close(child)
        targets.append(name)
    return targets


def _release_cleanup_targets(root: Optional[int]) -> tuple[list[str], list[str]]:
    if root is None:
        return [], []
    directories: list[str] = []
    pointers: list[str] = []
    generation_descriptor: Optional[int] = None
    for name in os.listdir(root):
        if name == ".current.generations":
            generation_descriptor = _open_child_directory(
                root, name, description="OTA release generation root"
            )
        elif name == ".current.build.lock":
            if not stat.S_ISREG(_metadata(root, name, description="OTA release build lock").st_mode):
                raise ValueError("OTA release build lock must be a regular file")
        elif name == "current":
            _contained_symlink(root, name)
        elif name.startswith(".current.current-"):
            _contained_symlink(root, name)
            pointers.append(name)
        else:
            raise ValueError("unexpected node in managed OTA release root")

    if generation_descriptor is not None:
        try:
            for name in os.listdir(generation_descriptor):
                generation = _open_child_directory(
                    generation_descriptor, name, description="OTA release generation"
                )
                try:
                    if name.startswith(".staging-"):
                        _require_release_staging_files(generation)
                        directories.append(name)
                    else:
                        _require_regular_files(
                            generation,
                    {"firmware.bin", "manifest.json", "manifest.sig"},
                    exact=True,
                        )
                finally:
                    os.close(generation)
        finally:
            os.close(generation_descriptor)
    return directories, pointers


def _remove_tree(parent: int, name: str) -> None:
    directory = _open_child_directory(parent, name, description="managed OTA cleanup target")
    original = os.fstat(directory)
    try:
        for child_name in os.listdir(directory):
            child = _metadata(directory, child_name, description="managed OTA cleanup child")
            if stat.S_ISLNK(child.st_mode):
                raise ValueError("managed OTA cleanup child must not be a symlink")
            if stat.S_ISDIR(child.st_mode):
                _remove_tree(directory, child_name)
            elif stat.S_ISREG(child.st_mode):
                os.unlink(child_name, dir_fd=directory)
            else:
                raise ValueError("managed OTA cleanup child has unsupported type")
        current = _metadata(parent, name, description="managed OTA cleanup target")
        if (current.st_dev, current.st_ino) != (original.st_dev, original.st_ino):
            raise ValueError("managed OTA cleanup target changed during cleanup")
        os.rmdir(name, dir_fd=parent)
    finally:
        os.close(directory)


def cleanup_stale_ota_runtime(
    *, snapshots_root: Path, sessions_root: Path, releases_root: Path
) -> None:
    """Remove only structurally recognized crash residue from controlled OTA roots."""

    roots = [
        _open_real_directory(Path(snapshots_root)),
        _open_real_directory(Path(sessions_root)),
        _open_real_directory(Path(releases_root)),
    ]
    snapshots, sessions, releases = roots
    try:
        snapshot_targets = _snapshot_cleanup_targets(snapshots)
        session_targets = _session_cleanup_targets(sessions)
        release_directories, release_pointers = _release_cleanup_targets(releases)
        # Validation is deliberately completed before the first deletion.
        if snapshots is not None:
            for name in snapshot_targets:
                _remove_tree(snapshots, name)
        if sessions is not None:
            for name in session_targets:
                _remove_tree(sessions, name)
        if releases is not None:
            generations = None
            if release_directories:
                generations = _open_child_directory(
                    releases, ".current.generations", description="OTA release generation root"
                )
            try:
                for name in release_directories:
                    assert generations is not None
                    _remove_tree(generations, name)
            finally:
                if generations is not None:
                    os.close(generations)
            for name in release_pointers:
                if not stat.S_ISLNK(
                    _metadata(releases, name, description="managed OTA pointer").st_mode
                ):
                    raise ValueError("managed OTA pointer changed during cleanup")
                os.unlink(name, dir_fd=releases)
    finally:
        for descriptor in roots:
            if descriptor is not None:
                os.close(descriptor)
