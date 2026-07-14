from dataclasses import replace
import json

from codex_buddy.events import ApprovalRequest, AgentOutput, TokenUsage, TurnState
from codex_buddy.reducer import BuddySnapshot, BuddyStateReducer
from codex_buddy.usage_limits import UsageDisplay


def test_turn_lifecycle_and_entries_are_projected_into_snapshot():
    reducer = BuddyStateReducer()

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=True))
    reducer.apply(AgentOutput(thread_id="thr_1", text="Thinking about tests"))
    reducer.apply(TokenUsage(thread_id="thr_1", total_tokens=120, tokens_today=45))

    snapshot = reducer.snapshot()

    assert snapshot.total == 1
    assert snapshot.running == 1
    assert snapshot.waiting == 0
    assert snapshot.msg == "Thinking about tests"
    assert snapshot.entries == ["Thinking about tests"]
    assert snapshot.tokens == 120
    assert snapshot.tokens_today == 45

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=False))

    stopped = reducer.snapshot()

    assert stopped.running == 0
    assert stopped.total == 0


def test_approval_request_populates_prompt_and_waiting_state():
    reducer = BuddyStateReducer()

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=True))
    reducer.apply(
        ApprovalRequest(
            thread_id="thr_1",
            turn_id="turn_1",
            request_id="req_1",
            command="rm -rf /tmp/foo",
            cwd="/tmp/project",
            reason="Needs approval",
        )
    )

    snapshot = reducer.snapshot()

    assert snapshot.running == 0
    assert snapshot.waiting == 1
    assert snapshot.msg == "approve: rm -rf /tmp/foo"
    assert snapshot.prompt == {
        "id": "req_1",
        "tool": "Bash",
        "hint": "rm -rf /tmp/foo",
    }

    reducer.resolve_approval("req_1")

    resolved = reducer.snapshot()

    assert resolved.waiting == 0
    assert resolved.running == 1
    assert resolved.prompt is None


def test_approval_request_clips_cjk_prompt_hint_by_display_width():
    reducer = BuddyStateReducer()
    command = "你" * 25

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=True))
    reducer.apply(
        ApprovalRequest(
            thread_id="thr_1",
            turn_id="turn_1",
            request_id="req_1",
            command=command,
            cwd="/tmp/project",
            reason="Needs approval",
        )
    )

    snapshot = reducer.snapshot()

    assert snapshot.prompt == {
        "id": "req_1",
        "tool": "Bash",
        "hint": command,
    }


def test_reducer_clips_cjk_entries_by_display_width():
    reducer = BuddyStateReducer()

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=True))
    reducer.apply(AgentOutput(thread_id="thr_1", text="你" * 50))

    snapshot = reducer.snapshot()

    assert snapshot.msg == ("你" * 21) + "…"
    assert snapshot.entries == ["你" * 50]


def test_ble_payload_uses_utf8_json_and_stays_within_device_budget_for_cjk_entries():
    reducer = BuddyStateReducer(tokens=51_163_764, tokens_today=303_102)

    reducer.apply(TurnState(thread_id="thr_1", turn_id="turn_1", active=True))
    reducer.apply(
        AgentOutput(
            thread_id="thr_1",
            text="编辑已经落地，我先做最小验证：`git diff --check` 看补丁质量，再跑一次 `xcodebuild build`，确认这次抽离没有把 SwiftUI 视图层级和项目编译打断。",
        )
    )
    reducer.apply(
        AgentOutput(
            thread_id="thr_1",
            text="补丁格式没问题。现在进编译验证，重点看新的书籍组件有没有触发 `some View` 推断、预览宏或 Xcode 项目文件里的连带问题。",
        )
    )
    reducer.apply(
        AgentOutput(
            thread_id="thr_1",
            text="编译还在跑，当前没有新的报错输出。这边我先盯到 Swift 编译结果出来，如果是类型推断或预览宏的问题会立刻改，不会把你丢在半成品状态。",
        )
    )

    snapshot = replace(
        reducer.snapshot(),
        usage=UsageDisplay(five_hour_remaining=72, seven_day_remaining=91),
    )
    payload = snapshot.as_ble_payload()

    utf8_line = json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    escaped_line = json.dumps(payload, separators=(",", ":")).encode("utf-8")

    assert snapshot.running == 1
    assert len(utf8_line) + 1 <= 900
    assert len(escaped_line) + 1 > 1024
    assert payload["entries"]
    assert payload["usage"] == {"five_hour_remaining": 72, "seven_day_remaining": 91}


def test_snapshot_usage_is_optional_and_keeps_the_compact_ble_shape():
    legacy_snapshot = BuddySnapshot(
        total=1,
        running=1,
        waiting=0,
        msg="Codex is working",
        entries=["Checking the change"],
        tokens=120,
        tokens_today=45,
        prompt=None,
    )

    assert legacy_snapshot.as_ble_payload() == {
        "total": 1,
        "running": 1,
        "waiting": 0,
        "msg": "Codex is working",
        "entries": ["Checking the change"],
        "tokens": 120,
        "tokens_today": 45,
    }

    snapshot_with_completion = replace(legacy_snapshot, completion_seq=7)
    assert snapshot_with_completion.as_ble_payload()["completion_seq"] == 7

    snapshot_with_usage = BuddySnapshot(
        total=1,
        running=1,
        waiting=0,
        msg="Codex is working",
        entries=["Checking the change"],
        tokens=120,
        tokens_today=45,
        prompt=None,
        usage=UsageDisplay(five_hour_remaining=72, seven_day_remaining=91),
    )

    assert snapshot_with_usage.as_ble_payload()["usage"] == {
        "five_hour_remaining": 72,
        "seven_day_remaining": 91,
    }

    snapshot_with_week_only = replace(
        snapshot_with_usage,
        usage=UsageDisplay(seven_day_remaining=99),
    )

    assert snapshot_with_week_only.as_ble_payload()["usage"] == {
        "seven_day_remaining": 99,
    }


def test_snapshot_emits_an_explicit_usage_clear_after_a_known_limit_expires():
    expired_snapshot = BuddySnapshot(
        total=1,
        running=1,
        waiting=0,
        msg="Codex is working",
        entries=["Checking the change"],
        tokens=120,
        tokens_today=45,
        prompt=None,
        usage=None,
        usage_is_known=True,
    )

    assert expired_snapshot.as_ble_payload()["usage"] is None
