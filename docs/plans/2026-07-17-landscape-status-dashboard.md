# Landscape Status Dashboard Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a display-only `RUN` / `ASK` / `NEW` dashboard to the StickS3 landscape clock, backed by local Codex task state and the Codex Desktop unread collection, while moving the clock down four pixels and replacing the landscape footer with one 20 px usage bar.

**Architecture:** The host keeps the current catalog as the source of `RUN` and `ASK`, and adds a defensive read-only watcher for Codex Desktop's local unread-thread state. The optional unread count travels through the existing snapshot/BLE contract; firmware validates and stores it, then a pure layout/render layer controls status formatting, colors, invalidation, and the landscape-only usage-meter geometry. Existing portrait, approval, OTA, menu, settings, passkey, and non-clock meter paths remain unchanged.

**Tech Stack:** Python 3.13, pytest, asyncio, JSON, Codex app-server/session catalog, BLE JSON, C++17, ArduinoJson, M5Unified/LovyanGFX, PlatformIO

---

Use `@superpowers:test-driven-development` for every task below. Before claiming completion, use `@superpowers:verification-before-completion` and keep host tests, native firmware tests, firmware build, install, and on-device observation as separate evidence.

### Task 1: Read Codex Desktop local unread state without mutating it

**Files:**
- Create: `src/codex_buddy/codex_client_state_watcher.py`
- Create: `tests/test_codex_client_state_watcher.py`

**Step 1: Write the failing watcher tests**

Cover these exact behaviors with temporary state files:

```python
def write_state(path: Path, local: object) -> None:
    path.write_text(json.dumps({
        "electron-persisted-atom-state": {
            "unread-thread-ids-by-host-v1": {"local": local}
        }
    }))


def test_counts_unique_local_unread_thread_ids(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    write_state(path, ["thread-a", "thread-b"])
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll() == 2


def test_missing_or_malformed_state_is_unknown_before_first_valid_read(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll() is None
    path.write_text("{")
    assert watcher.poll() is None


def test_transient_failure_retains_last_trusted_count(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    write_state(path, ["thread-a"])
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll() == 1
    path.write_text("{")
    assert watcher.poll() == 1


def test_rejects_non_string_and_duplicate_ids_without_replacing_trusted_value(tmp_path: Path) -> None:
    path = tmp_path / ".codex-global-state.json"
    write_state(path, ["thread-a"])
    watcher = CodexClientStateWatcher(path)
    assert watcher.poll() == 1
    write_state(path, ["thread-a", "thread-a"])
    assert watcher.poll() == 1
    write_state(path, ["thread-a", 7])
    assert watcher.poll() == 1
```

Also assert the watcher follows only `...unread-thread-ids-by-host-v1.local`; remote-host arrays must not affect the count. Do not add any write or mark-read API.

**Step 2: Run the tests to verify they fail**

Run:

```bash
PYTHONPATH=src .venv/bin/pytest -q tests/test_codex_client_state_watcher.py
```

Expected: FAIL during import because `codex_client_state_watcher.py` does not exist.

**Step 3: Implement the minimal defensive watcher**

Implement a small synchronous class suitable for the agent's existing two-second polling loop:

```python
class CodexClientStateWatcher:
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
```

Keep the class read-only. Avoid logging on every poll; let the agent log only bounded state transitions if diagnostic logging is needed.

**Step 4: Run the watcher tests to verify they pass**

Run:

```bash
PYTHONPATH=src .venv/bin/pytest -q tests/test_codex_client_state_watcher.py
```

Expected: all watcher tests PASS.

**Step 5: Commit**

```bash
git add src/codex_buddy/codex_client_state_watcher.py tests/test_codex_client_state_watcher.py
git commit -m "feat: read Codex client unread state"
```

### Task 2: Publish optional unread count through the host snapshot

**Files:**
- Modify: `src/codex_buddy/reducer.py:27-65`
- Modify: `src/codex_buddy/agent.py:185-225,546-553,698-705`
- Modify: `tests/test_reducer.py`
- Modify: `tests/test_agent.py`

**Step 1: Write failing snapshot and agent tests**

Add reducer tests showing that `BuddySnapshot(unread=None)` omits the key for backward compatibility and `BuddySnapshot(unread=3)` emits `"unread": 3` without changing existing payload fields.

Add an injectable unread watcher fake to `tests/test_agent.py`:

```python
class FakeUnreadWatcher:
    def __init__(self, values: list[Optional[int]]) -> None:
        self.values = iter(values)

    def poll(self) -> Optional[int]:
        return next(self.values)
```

Exercise one readonly-loop iteration or the extracted poll helper and assert:

```python
assert agent._snapshot().unread == 4
```

Also cover `None` before the first trusted read and a subsequent changed count. The watcher itself owns transient-value retention, so the agent should only copy the result.

**Step 2: Run focused host tests to verify they fail**

Run:

```bash
PYTHONPATH=src .venv/bin/pytest -q tests/test_reducer.py tests/test_agent.py
```

Expected: FAIL because `BuddySnapshot` and `BuddyAgent` do not accept or publish unread state.

**Step 3: Add the optional snapshot field and agent integration**

In `BuddySnapshot`, append a defaulted field so existing constructors remain valid:

```python
unread: Optional[int] = None
```

In `as_ble_payload()`:

```python
if self.unread is not None:
    payload["unread"] = self.unread
```

In `BuddyAgent.__init__`, add an optional `client_state_watcher` injection and default it to:

```python
CodexClientStateWatcher(Path.home() / ".codex" / ".codex-global-state.json")
```

Initialize `self._unread: Optional[int] = None`. Poll the client watcher from the existing `_readonly_loop` regardless of whether session-log discovery is enabled, update `self._unread`, and publish once after both local session and unread state have been sampled. Extend `_snapshot()` with `unread=self._unread`.

Do not derive `NEW` from completed tasks, do not include remote hosts, and do not expose a device-side clear operation.

**Step 4: Run focused and full host tests**

Run:

```bash
PYTHONPATH=src .venv/bin/pytest -q tests/test_codex_client_state_watcher.py tests/test_reducer.py tests/test_agent.py
UV_PROJECT_ENVIRONMENT=/tmp/codebuddy-py313-test uv run --python 3.13 --extra dev pytest -q
```

Expected: focused tests PASS; full Python 3.13 suite PASS.

**Step 5: Commit**

```bash
git add src/codex_buddy/reducer.py src/codex_buddy/agent.py tests/test_reducer.py tests/test_agent.py
git commit -m "feat: publish unread task count"
```

### Task 3: Validate and retain unread count in firmware state

**Files:**
- Create: `firmware/src/status_dashboard_logic.h`
- Create: `firmware/tests/status_dashboard_logic_test.cpp`
- Modify: `firmware/src/data.h:12-30,260-280`
- Modify: `firmware/tests/data_usage_logic_test.cpp` or the existing native JSON-state test that includes `data.h`

**Step 1: Write failing pure-logic tests**

Define and test a compact state helper independent of the screen library:

```cpp
StatusDashboardCounts counts = {};
statusDashboardApplyUnread(&counts, true, true, 7);
assert(counts.hasUnread);
assert(counts.unread == 7);

statusDashboardApplyUnread(&counts, false, false, 0);
assert(counts.unread == 7);  // missing field preserves compatibility

statusDashboardApplyUnread(&counts, true, false, 300);
assert(counts.unread == 7);  // malformed field cannot corrupt trusted state

char text[4] = {};
statusDashboardFormatCount(text, sizeof(text), 0);
assert(strcmp(text, "0") == 0);
statusDashboardFormatCount(text, sizeof(text), 99);
assert(strcmp(text, "99") == 0);
statusDashboardFormatCount(text, sizeof(text), 100);
assert(strcmp(text, "99+") == 0);
```

Add color-role tests: zero maps to dim, positive `RUN` to green, positive `ASK` to amber, and positive `NEW` to cyan. Keep colors as named semantic roles in pure logic and translate them to RGB565 in rendering.

**Step 2: Run the native test to verify it fails**

Run:

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/status_dashboard_logic_test.cpp -o /tmp/status_dashboard_logic_test
/tmp/status_dashboard_logic_test
```

Expected: compilation FAIL because `status_dashboard_logic.h` does not exist.

**Step 3: Implement pure status helpers and wire JSON parsing**

Add:

```cpp
enum StatusDashboardKind : uint8_t { STATUS_RUN, STATUS_ASK, STATUS_NEW };
enum StatusDashboardColorRole : uint8_t {
  STATUS_COLOR_DIM, STATUS_COLOR_GREEN, STATUS_COLOR_AMBER, STATUS_COLOR_CYAN
};

struct StatusDashboardCounts {
  bool hasUnread;
  uint8_t unread;
};
```

Implement `statusDashboardApplyUnread`, `statusDashboardFormatCount`, and `statusDashboardColorRole`. In `TamaState`, store `hasUnreadCount` plus `unreadCount`. In `_applyJson`, inspect `doc["unread"]`; only call the trusted update path when the variant is an integer in `0...255`. Missing and malformed values leave the prior trusted value unchanged. Ensure reset/default construction starts at unknown/zero.

Do not reject the rest of a valid heartbeat because only `unread` is malformed.

**Step 4: Run native tests**

Run:

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/status_dashboard_logic_test.cpp -o /tmp/status_dashboard_logic_test
/tmp/status_dashboard_logic_test
```

Then run the repository's existing JSON/state test command for `data.h` (use its established PlatformIO/ArduinoJson include setup) and confirm old payloads without `unread` still pass.

Expected: all targeted tests PASS.

**Step 5: Commit**

```bash
git add firmware/src/status_dashboard_logic.h firmware/src/data.h firmware/tests/status_dashboard_logic_test.cpp firmware/tests/*data*_test.cpp
git commit -m "feat: accept unread count on device"
```

### Task 4: Lock the new landscape geometry and single 20 px meter

**Files:**
- Modify: `firmware/src/shared_clock_face_logic.h:20-130`
- Modify: `firmware/src/usage_meter_logic.h:1-245`
- Modify: `firmware/tests/shared_clock_face_logic_test.cpp`
- Modify: `firmware/tests/usage_meter_logic_test.cpp`

**Step 1: Change layout tests first**

Update the landscape assertions to the approved pixel geometry:

```cpp
const SharedClockFaceLayout landscape = sharedClockFaceLayout(true);
assert(landscape.pet.x == 0);
assert(landscape.pet.y == 0);
assert(landscape.pet.width == 120);
assert(landscape.pet.height == 58);
assert(landscape.status.x == 120);
assert(landscape.status.y == 0);
assert(landscape.status.width == 120);
assert(landscape.status.height == 58);
assert(landscape.status.columnWidth == 40);
assert(landscape.time.primary.y == 77);
assert(landscape.time.seconds.y == 77);
assert(landscape.time.centerY == 93);
assert(landscape.date.month.y == 74);
assert(landscape.date.day.y == 94);
assert(landscape.meterY == 111);
assert(landscape.meterFootprint == 24);
```

Assert every portrait value remains exactly as before and that portrait has no status surface.

Add a landscape-meter test:

```cpp
UsageMeterRenderPlan plan = usageMeterLandscapeSinglePlan(usage, 240, 135);
assert(plan.count == 2);
assert(plan.rects[0].x == 2);
assert(plan.rects[0].y == 113);
assert(plan.rects[0].width == 236);
assert(plan.rects[0].height == 20);
assert(plan.rects[1].height == 20);
```

For two available windows, assert this landscape-only plan chooses the seven-day value/color. Also test seven-day-only, five-hour fallback when seven-day is absent, invalid/unavailable usage, and 0/100 percent fill widths. Existing generic one/two-bar tests must remain unchanged.

**Step 2: Run tests to verify the new expectations fail**

Run:

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/shared_clock_face_logic_test.cpp -o /tmp/shared_clock_face_logic_test
/tmp/shared_clock_face_logic_test
clang++ -std=c++17 -Ifirmware/src firmware/tests/usage_meter_logic_test.cpp -o /tmp/usage_meter_logic_test
/tmp/usage_meter_logic_test
```

Expected: assertion or compilation failures for the old layout and missing landscape-specific plan.

**Step 3: Implement only the landscape layout and plan**

Add a `SharedClockStatusLayout` to `SharedClockFaceLayout`, use a 120 x 58 right-hand region with three 40 px columns, and apply the exact four-pixel clock/date offsets. Leave the portrait initializer byte-for-byte equivalent aside from the new empty status field.

Add landscape-only constants and a helper without changing the existing global meter constants:

```cpp
static constexpr uint8_t LANDSCAPE_USAGE_METER_HEIGHT = 20;
static constexpr uint8_t LANDSCAPE_USAGE_METER_FOOTPRINT = 24;
static constexpr uint8_t LANDSCAPE_USAGE_METER_TOP_INSET = 2;

inline UsageMeterRenderPlan usageMeterLandscapeSinglePlan(
  const UsageMeterState& state,
  uint16_t fullWidth,
  uint16_t fullHeight
);
```

Use x `2`, y `fullHeight - 22`, width `fullWidth - 4`, height `20`. Prefer seven-day when present because the approved single bar represents the longer account quota; use five-hour only as a compatibility fallback. Preserve existing consumed background and selected-window fill colors.

**Step 4: Run geometry and meter tests**

Run the two commands from Step 2.

Expected: both executables exit 0; existing portrait and generic meter assertions still PASS.

**Step 5: Commit**

```bash
git add firmware/src/shared_clock_face_logic.h firmware/src/usage_meter_logic.h firmware/tests/shared_clock_face_logic_test.cpp firmware/tests/usage_meter_logic_test.cpp
git commit -m "feat: define landscape dashboard layout"
```

### Task 5: Render status counts with independent invalidation

**Files:**
- Modify: `firmware/src/shared_clock_face_logic.h:75-110`
- Modify: `firmware/src/clock_display_logic.h:113-175`
- Modify: `firmware/src/main.cpp:180-210,802-980`
- Modify: `firmware/tests/shared_clock_face_logic_test.cpp`
- Modify: `firmware/tests/clock_display_logic_test.cpp`

**Step 1: Write failing invalidation tests**

Extend render-input/decision tests so:

```cpp
SharedClockFaceRenderInput input = {};
input.statusChanged = true;
SharedClockFaceRenderDecision decision = sharedClockFaceRenderDecision(input);
assert(decision.drawStatus);
assert(!decision.clearSurface);
assert(!decision.drawPet);
assert(!decision.drawTime);
assert(!decision.drawDate);
```

Assert first entry, orientation change, and full repaint draw status in landscape. Assert count-only changes do not draw status in portrait. Extend `SharedClockFaceCache` scheduling tests so identical `running`, `waiting`, and `unread` values do not redraw; changing one value redraws only the status region.

**Step 2: Run invalidation tests to verify they fail**

Run:

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/shared_clock_face_logic_test.cpp -o /tmp/shared_clock_face_logic_test
/tmp/shared_clock_face_logic_test
clang++ -std=c++17 -Ifirmware/src firmware/tests/clock_display_logic_test.cpp -o /tmp/clock_display_logic_test
/tmp/clock_display_logic_test
```

Expected: compilation FAIL because status invalidation fields do not exist.

**Step 3: Implement cache keys and landscape rendering**

Add `lastRunning`, `lastWaiting`, and `lastUnread` to `SharedClockFaceCache`. Extend `clockSharedFaceSchedule` with the three counts, calculate `statusChanged`, and update cached values after making the decision. Pass `tama.sessionsRunning`, `tama.sessionsWaiting`, and `tama.hasUnreadCount ? tama.unreadCount : 0` from `drawSharedClockFaceTo`.

Add `drawStatus` to `SharedClockFaceRenderDecision`. On landscape only:

1. Clear exactly x `120...239`, y `0...57` with the palette background.
2. Center `RUN`, `ASK`, and `NEW` in their 40 px columns at text size 1 using dim text.
3. Format counts with `statusDashboardFormatCount` and center them below the labels at text size 2.
4. Resolve semantic roles to RGB565: RUN bright green, ASK amber/orange, NEW cyan, and every zero dim gray.
5. Do not clear or redraw the pet region during a count-only update.

Route only the landscape shared clock through a new `usageMeterPrepareLandscapeSingleFrame` (or an equivalent plan-injected preparation helper). Continue to call the existing generic `usageMeterFrameForDisplay` everywhere else. On full repaint, clear and redraw the new 24 px footer; on usage-only updates, touch only y `111...134`.

**Step 4: Run all native logic tests and build firmware**

Run:

```bash
for test in firmware/tests/*_test.cpp; do
  bin="/tmp/$(basename "${test%.cpp}")"
  clang++ -std=c++17 -Ifirmware/src "$test" -o "$bin" && "$bin"
done
cd firmware && pio run
```

If an established ArduinoJson test needs PlatformIO include flags, run it with the repository's existing command instead of weakening/skipping it.

Expected: every native test exits 0 and PlatformIO reports `SUCCESS` for the StickS3 environment.

**Step 5: Commit**

```bash
git add firmware/src/shared_clock_face_logic.h firmware/src/clock_display_logic.h firmware/src/main.cpp firmware/tests/shared_clock_face_logic_test.cpp firmware/tests/clock_display_logic_test.cpp
git commit -m "feat: render landscape task dashboard"
```

### Task 6: Verify the full host-to-device behavior

**Files:**
- Modify only if a verification-discovered defect requires a minimal fix.

**Step 1: Run the complete automated verification set**

Run:

```bash
UV_PROJECT_ENVIRONMENT=/tmp/codebuddy-py313-test uv run --python 3.13 --extra dev pytest -q
for test in firmware/tests/*_test.cpp; do
  bin="/tmp/$(basename "${test%.cpp}")"
  clang++ -std=c++17 -Ifirmware/src "$test" -o "$bin" && "$bin"
done
cd firmware && pio run
```

Expected: full host suite PASS, every supported native logic test PASS, and firmware build `SUCCESS`.

**Step 2: Verify the live source state before touching hardware**

Read the local array length without modifying the file:

```bash
jq '."electron-persisted-atom-state"."unread-thread-ids-by-host-v1".local | length' ~/.codex/.codex-global-state.json
```

Start/repair Code Buddy using the worktree build, then confirm the persisted/published snapshot includes the same `unread` count. Verify `RUN` and `ASK` still match local catalog state.

**Step 3: Install the firmware using the currently validated device path**

Prefer the repository's supported OTA update when the paired device is online; otherwise use the documented USB fallback:

```bash
cd firmware
pio run -t upload --upload-port /dev/cu.usbmodem101
```

Record build success, transfer/install success, firmware boot/readback, and actual UI behavior as separate facts.

**Step 4: Perform visual and semantic device checks**

On the physical 240 x 135 screen, verify:

- Top region is an even 120/120 split; pet animation never erases status text.
- `RUN`, `ASK`, and `NEW` are centered and legible; positive colors are green, amber, and cyan respectively; zeros are dim.
- Opening an unread local task in Codex Desktop reduces `NEW` after the next host poll/BLE publish.
- Device buttons do not alter `NEW`.
- Time/date are exactly four pixels lower and do not overlap the footer.
- Exactly one bar appears at x 2, y 113, width 236, height 20.
- Rotating to portrait and showing approval, OTA, menu, settings, and passkey screens reveals no geometry regression.

Capture a screen photo plus host snapshot/log evidence for the final handoff.

**Step 5: Commit only verification fixes, if any**

If verification required a code fix, repeat the failing test first and make a focused commit. If no defect was found, do not create an empty commit.

**Step 6: Final review**

Run:

```bash
git status --short
git log --oneline --decorate -8
```

Expected: only the pre-existing untracked `build/` remains; all implementation changes are committed. Report automated, install, boot, and visual evidence separately.
