import asyncio
import json

import pytest

from codex_buddy.events import ApprovalRequest, TurnState
from codex_buddy.proxy import CodexEventSource, map_device_decision_to_codex_response


class _FakeUpstream:
    def __init__(self) -> None:
        self.messages: list[dict[str, object]] = []

    async def send(self, raw: str) -> None:
        self.messages.append(json.loads(raw))


@pytest.mark.parametrize(
    ("device_decision", "expected"),
    [
        ("once", {"decision": "accept"}),
        ("deny", {"decision": "decline"}),
    ],
)
def test_device_decisions_map_to_codex_approval_payloads(device_decision, expected):
    assert map_device_decision_to_codex_response(device_decision) == expected


def test_unknown_device_decision_is_rejected():
    with pytest.raises(ValueError, match="Unsupported device decision"):
        map_device_decision_to_codex_response("session")


@pytest.mark.parametrize(
    ("turn", "expected_status"),
    [
        ({"id": "turn_1", "status": "failed"}, "failed"),
        ({"id": "turn_1"}, "completed"),
    ],
)
def test_turn_completed_preserves_terminal_status(turn, expected_status):
    async def exercise() -> list[object]:
        events: list[object] = []

        async def on_event(event: object) -> None:
            events.append(event)

        source = CodexEventSource(
            upstream_url="ws://example.test",
            listen_host="127.0.0.1",
            listen_port=0,
            on_event=on_event,
        )

        await source._emit_events(
            {
                "method": "turn/completed",
                "params": {"threadId": "thr_1", "turn": turn},
            }
        )
        return events

    assert asyncio.run(exercise()) == [
        TurnState(
            thread_id="thr_1",
            turn_id="turn_1",
            active=False,
            status=expected_status,
        )
    ]


def test_read_only_verification_after_approved_mutation_is_auto_accepted():
    async def exercise() -> tuple[list[object], list[dict[str, object]]]:
        events: list[object] = []
        
        async def on_event(event: object) -> None:
            events.append(event)

        source = CodexEventSource(
            upstream_url="ws://example.test",
            listen_host="127.0.0.1",
            listen_port=0,
            on_event=on_event,
        )
        source._upstream = _FakeUpstream()

        await source._emit_events(
            {
                "id": "1",
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "threadId": "thr_1",
                    "turnId": "turn_1",
                    "command": "/bin/zsh -lc 'rm -f /tmp/demo'",
                    "cwd": "/tmp",
                    "reason": "",
                },
            }
        )
        await source.respond_to_device_approval("1", "once")

        await source._emit_events(
            {
                "id": "2",
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "threadId": "thr_1",
                    "turnId": "turn_1",
                    "command": "/bin/zsh -lc 'test ! -e /tmp/demo && echo removed'",
                    "cwd": "/tmp",
                    "reason": "",
                },
            }
        )
        return events, source._upstream.messages

    events, messages = asyncio.run(exercise())

    assert [event for event in events if isinstance(event, ApprovalRequest)] == [
        ApprovalRequest(
            thread_id="thr_1",
            turn_id="turn_1",
            request_id="1",
            command="/bin/zsh -lc 'rm -f /tmp/demo'",
            cwd="/tmp",
            reason="",
        )
    ]
    assert messages == [
        {"id": 1, "result": {"decision": "accept"}},
        {"id": 2, "result": {"decision": "accept"}},
    ]


def test_read_only_verification_without_prior_mutation_still_requires_approval():
    async def exercise() -> tuple[list[object], list[dict[str, object]]]:
        events: list[object] = []

        async def on_event(event: object) -> None:
            events.append(event)

        source = CodexEventSource(
            upstream_url="ws://example.test",
            listen_host="127.0.0.1",
            listen_port=0,
            on_event=on_event,
        )
        source._upstream = _FakeUpstream()

        await source._emit_events(
            {
                "id": "1",
                "method": "item/commandExecution/requestApproval",
                "params": {
                    "threadId": "thr_1",
                    "turnId": "turn_1",
                    "command": "/bin/zsh -lc 'test ! -e /tmp/demo && echo removed'",
                    "cwd": "/tmp",
                    "reason": "",
                },
            }
        )
        return events, source._upstream.messages

    events, messages = asyncio.run(exercise())

    assert [event for event in events if isinstance(event, ApprovalRequest)] == [
        ApprovalRequest(
            thread_id="thr_1",
            turn_id="turn_1",
            request_id="1",
            command="/bin/zsh -lc 'test ! -e /tmp/demo && echo removed'",
            cwd="/tmp",
            reason="",
        )
    ]
    assert messages == []
