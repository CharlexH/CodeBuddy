from __future__ import annotations

import json
from pathlib import Path
from typing import Optional


class CodexClientStateWatcher:
    """Read Codex Desktop's local unread collection without mutating it."""

    def __init__(self, state_path: Path) -> None:
        self.state_path = state_path
        self._last_trusted: Optional[int] = None

    def poll(self) -> Optional[int]:
        try:
            root = json.loads(self.state_path.read_text(encoding="utf-8"))
            persisted = root["electron-persisted-atom-state"]
            unread_by_host = persisted["unread-thread-ids-by-host-v1"]
            local = unread_by_host["local"]
            if not isinstance(local, list):
                raise ValueError("local unread state is not a list")
            if not all(isinstance(thread_id, str) for thread_id in local):
                raise ValueError("local unread state contains a non-string id")
            if len(set(local)) != len(local):
                raise ValueError("local unread state contains duplicate ids")
        except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError, ValueError):
            return self._last_trusted

        self._last_trusted = len(local)
        return self._last_trusted
