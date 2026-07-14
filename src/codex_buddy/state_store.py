from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Optional


@dataclass(frozen=True)
class PersistedState:
    paired_device_id: Optional[str] = None
    paired_device_name: Optional[str] = None
    tokens_today: int = 0
    tokens_date: str = ""
    tokens_total: int = 0
    completion_seq: int = 0
    active_thread_id: Optional[str] = None
    buddy_connected: bool = False
    last_msg: str = ""
    snapshot: dict[str, Any] = field(default_factory=dict)
    sessions: list[dict[str, Any]] = field(default_factory=list)
    agent_running: bool = False
    setup_version: int = 0
    real_codex_path: str = ""
    codex_launch_path: str = ""
    helper_app_path: str = ""
    shim_dir: str = ""
    shell_integrated: bool = False
    service_installed: bool = False


class BridgeStateStore:
    def __init__(self, path: Path) -> None:
        self.path = path

    def load(self, *, now: Optional[datetime] = None) -> PersistedState:
        state = self._read()
        if now is None:
            now = datetime.now().astimezone()
        today = now.date().isoformat()
        if state.tokens_date != today:
            state = PersistedState(
                paired_device_id=state.paired_device_id,
                paired_device_name=state.paired_device_name,
                tokens_today=0,
                tokens_date=today,
                tokens_total=state.tokens_total,
                completion_seq=state.completion_seq,
                active_thread_id=state.active_thread_id,
                buddy_connected=state.buddy_connected,
                last_msg=state.last_msg,
                snapshot=state.snapshot,
                sessions=state.sessions,
                agent_running=state.agent_running,
                setup_version=state.setup_version,
                real_codex_path=state.real_codex_path,
                codex_launch_path=state.codex_launch_path,
                helper_app_path=state.helper_app_path,
                shim_dir=state.shim_dir,
                shell_integrated=state.shell_integrated,
                service_installed=state.service_installed,
            )
        return state

    def save(self, state: PersistedState) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(asdict(state), indent=2, sort_keys=True) + "\n")

    def _read(self) -> PersistedState:
        if not self.path.exists():
            return PersistedState()
        data = json.loads(self.path.read_text())
        completion_seq = data.get("completion_seq")
        if "completion_seq" not in data:
            snapshot = data.get("snapshot")
            if isinstance(snapshot, dict):
                completion_seq = snapshot.get("completion_seq")
        if type(completion_seq) is not int or not 0 <= completion_seq <= 0xFFFFFFFF:
            completion_seq = 0
        data["completion_seq"] = completion_seq
        return PersistedState(**data)
