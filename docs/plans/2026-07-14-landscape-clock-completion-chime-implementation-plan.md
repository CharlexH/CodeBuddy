# Landscape Clock and Completion Chime Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enlarge and restructure the StickS3 landscape clock, then play one compact two-note sound for every successfully completed managed Codex turn.

**Architecture:** Keep the existing shared clock renderer and change only its landscape geometry. Carry a persisted monotonic `completion_seq` from app-server `turn/completed` events through the agent snapshot to firmware; firmware establishes a baseline on first receipt and plays once for each later sequence change.

**Tech Stack:** Python 3.13, pytest, Codex app-server websocket events, JSON over BLE NUS, C++17 pure firmware tests, ArduinoJson, M5Unified, PlatformIO.

---

### Task 1: Preserve terminal turn status and emit a deduplicated completion sequence

**Files:**
- Modify: `src/codex_buddy/events.py`
- Modify: `src/codex_buddy/proxy.py`
- Modify: `src/codex_buddy/reducer.py`
- Modify: `src/codex_buddy/agent.py`
- Test: `tests/test_proxy.py`
- Test: `tests/test_reducer.py`
- Test: `tests/test_agent.py`

**Step 1: Write the failing proxy and agent tests**

Add tests proving:

```python
assert TurnState(
    thread_id="thr-1",
    turn_id="turn-1",
    active=False,
    status="completed",
) in events

assert agent._snapshot().as_ble_payload()["completion_seq"] == 1
```

Also prove duplicate terminal events do not increment twice, and `interrupted` / `failed` terminal statuses do not increment the success sequence.

**Step 2: Run the focused tests and verify RED**

Run:

```bash
PYTHONPATH=src .venv313/bin/pytest -q tests/test_proxy.py tests/test_reducer.py tests/test_agent.py
```

Expected: failures because `TurnState` has no terminal status and snapshots have no `completion_seq`.

**Step 3: Add minimal status and sequence behavior**

- Add a backward-compatible `status: str = ""` field to `TurnState`.
- Parse `params.turn.status` for `turn/completed`; default missing status to `completed` for older app-server compatibility.
- Add optional `completion_seq` serialization to `BuddySnapshot`.
- Initialize the agent sequence from persisted state.
- On a unique `(thread_id, turn_id)` successful terminal event, increment modulo `2^32` before publishing.
- Keep a bounded recent-turn key collection so duplicate events cannot retrigger while memory remains bounded.
- Do not react to `item/completed`.

**Step 4: Run focused tests and verify GREEN**

Run the command from Step 2.

Expected: all focused tests pass.

**Step 5: Commit**

```bash
git add src/codex_buddy/events.py src/codex_buddy/proxy.py src/codex_buddy/reducer.py src/codex_buddy/agent.py tests/test_proxy.py tests/test_reducer.py tests/test_agent.py
git commit -m "feat: publish completed turn sequence"
```

### Task 2: Persist the completion sequence across host restarts

**Files:**
- Modify: `src/codex_buddy/state_store.py`
- Modify: `src/codex_buddy/agent.py`
- Modify: `src/codex_buddy/bridge.py`
- Test: `tests/test_state_store.py`
- Test: `tests/test_agent.py`

**Step 1: Write failing persistence tests**

Add assertions that `completion_seq` round-trips, survives the local-midnight token reset, is loaded by a new `BuddyAgent`, and is written with each agent snapshot.

**Step 2: Run tests and verify RED**

```bash
PYTHONPATH=src .venv313/bin/pytest -q tests/test_state_store.py tests/test_agent.py
```

Expected: failures because `PersistedState` does not contain `completion_seq`.

**Step 3: Implement persistence**

- Add `completion_seq: int = 0` to `PersistedState`.
- Preserve it in the explicit midnight-reset copy.
- Load it during `BuddyAgent` construction.
- Save it from `BuddyAgent._persist()`.
- Preserve the current value in the legacy bridge save path so legacy writes cannot reset the counter.

**Step 4: Run tests and verify GREEN**

Run the command from Step 2.

Expected: all tests pass.

**Step 5: Commit**

```bash
git add src/codex_buddy/state_store.py src/codex_buddy/agent.py src/codex_buddy/bridge.py tests/test_state_store.py tests/test_agent.py
git commit -m "feat: persist completed turn sequence"
```

### Task 3: Parse and deduplicate completion sequences in firmware

**Files:**
- Create: `firmware/src/completion_chime_logic.h`
- Create: `firmware/tests/completion_chime_logic_test.cpp`
- Modify: `firmware/src/data.h`
- Modify: `firmware/src/main.cpp`

**Step 1: Write the failing pure C++ test**

Define the intended state machine:

```cpp
CompletionChimeState state = {};
expect_true(!completionChimeObserve(&state, true, 7), "first sequence establishes baseline");
expect_true(!completionChimeObserve(&state, true, 7), "duplicate snapshot stays silent");
expect_true(completionChimeObserve(&state, true, 8), "next completed turn chimes once");
expect_true(!completionChimeObserve(&state, true, 8), "repeated completion stays silent");
expect_true(completionChimeObserve(&state, true, 0), "sequence wrap remains a change");
```

Also prove a missing sequence does not initialize or chime.

**Step 2: Compile and verify RED**

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/completion_chime_logic_test.cpp -o /tmp/completion_chime_logic_test
```

Expected: compile failure because `completion_chime_logic.h` does not exist.

**Step 3: Add the minimal pure state machine**

Create a header-only `CompletionChimeState` plus `completionChimeObserve(...)` implementation using first-value baseline and inequality-based wrap-safe change detection.

**Step 4: Compile, run, and verify GREEN**

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/completion_chime_logic_test.cpp -o /tmp/completion_chime_logic_test
/tmp/completion_chime_logic_test
```

Expected: exit code `0`.

**Step 5: Integrate JSON and sound**

- Add `hasCompletionSeq` and `completionSeq` to `TamaState`.
- Accept only an unsigned integer `completion_seq` from snapshots; preserve the previous value when the member is absent.
- Observe the state once per loop after `dataPoll()`.
- Add `playCompletionSound()` that checks `settings().sound`, then queues `1600 Hz / 70 ms` and `2400 Hz / 100 ms` on one M5Unified speaker channel with `stop_current_sound=false` for the second tone.
- Do not block with `delay()` and do not wake the screen.

**Step 6: Run the test again and commit**

```bash
git add firmware/src/completion_chime_logic.h firmware/tests/completion_chime_logic_test.cpp firmware/src/data.h firmware/src/main.cpp
git commit -m "feat: play completed turn chime"
```

### Task 4: Replace the landscape shared-clock geometry

**Files:**
- Modify: `firmware/src/shared_clock_face_logic.h`
- Modify: `firmware/src/main.cpp`
- Test: `firmware/tests/shared_clock_face_logic_test.cpp`

**Step 1: Change the layout test first**

Pin the accepted geometry:

```cpp
expect_true(landscape.pet.x == 50 && landscape.pet.y == 0 &&
            landscape.pet.width == 140 && landscape.pet.height == 58,
            "landscape pet should be centered across the top");
expect_true(landscape.time.primary.x == 8 && landscape.time.textSize == 4,
            "landscape HH:MM should use portrait-scale type on the left");
expect_true(landscape.date.monthCenterY < landscape.date.dayCenterY,
            "month should stack above day on the right");
expect_true(landscape.meterY == 119 && landscape.meterFootprint == 16,
            "landscape meters should retain their bottom footprint");
```

Extend `SharedClockDateLayout` as needed to carry separate month/day coordinates and a stacked/numeric landscape policy while retaining the portrait date line.

**Step 2: Compile and run the test to verify RED**

```bash
clang++ -std=c++17 -Ifirmware/src firmware/tests/shared_clock_face_logic_test.cpp -o /tmp/shared_clock_face_logic_test
/tmp/shared_clock_face_logic_test
```

Expected: assertion or compile failure against the old left-pet/right-time layout.

**Step 3: Implement the minimal landscape layout**

- Keep portrait constants and rendering unchanged.
- Use landscape pet rectangle `{50, 0, 140, 58}`.
- Render `HH:MM` at text size `4`, left aligned at `x=8` within `y=60..118`.
- Do not render landscape seconds.
- Render zero-padded numeric month above zero-padded numeric day at text size `2` near the right edge.
- Keep meter placement at `y=119`.
- Ensure pet refresh clears only its new rectangle.

**Step 4: Compile, run, and verify GREEN**

Run the command from Step 2.

Expected: exit code `0`.

**Step 5: Commit**

```bash
git add firmware/src/shared_clock_face_logic.h firmware/src/main.cpp firmware/tests/shared_clock_face_logic_test.cpp
git commit -m "feat: enlarge landscape clock layout"
```

### Task 5: Full verification

**Files:**
- Verify only; fix only failures caused by this feature.

**Step 1: Run all host tests**

```bash
PYTHONPATH=src .venv313/bin/pytest -q
```

Expected: all tests pass; existing `websockets` deprecation warnings may remain.

**Step 2: Run every pure firmware test**

```bash
for test in firmware/tests/*_test.cpp; do
  bin="/tmp/$(basename "$test" .cpp)"
  clang++ -std=c++17 -Ifirmware/src "$test" -o "$bin" && "$bin"
done
```

Expected: every binary exits `0`.

**Step 3: Build the complete firmware**

```bash
cd firmware && pio run
```

Expected: PlatformIO reports `SUCCESS` for `m5stack-sticks3`.

**Step 4: Review the final diff**

```bash
git diff --check
git status --short
git log --oneline -8
```

Expected: no whitespace errors and no unintended files.

**Step 5: Record validation without claiming hardware proof**

Report host tests, firmware tests, and build separately. Do not claim physical screen geometry, speaker playback, flash, or BLE readback until the StickS3 is explicitly deployed and observed.
