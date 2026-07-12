# Shared Standby and Runtime Clock Face Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Render standby and normal active Codex states through one pet + single-line time + date + usage-meter component.

**Architecture:** Introduce pure geometry/scheduling helpers, then refactor the existing charging clock and pet-only runtime to call a shared renderer with separate explicit cache instances. Functional overlays retain priority and meters remain the final layer.

**Tech Stack:** C++17 pure host tests, M5Unified/LovyanGFX, existing clock/pet/usage renderers, PlatformIO.

---

### Task 1: Define shared face geometry and scheduling

**Files:**
- Create: `firmware/src/shared_clock_face_logic.h`
- Create: `firmware/tests/shared_clock_face_logic_test.cpp`

**Steps:**
1. Write failing tests for portrait and landscape pet/time/date/meter-safe bounds, identical time segment size, seconds dim role, invalid-time eligibility, idle/active selection, approval and functional-screen exclusion, first-entry/full repaint, one-second text cadence, 200 ms pet cadence, and prompt-exit repaint.
2. Run the focused g++ test and confirm the missing-helper RED failure.
3. Implement only pure structs/functions and constants.
4. Run focused test and all firmware host C++ tests.
5. Commit as `feat: define shared clock face layout`.

### Task 2: Refactor standby and runtime onto the shared renderer

**Files:**
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/clock_display_logic.h`
- Modify: `firmware/tests/clock_display_logic_test.cpp`
- Modify: `firmware/tests/screen_orient_logic_test.cpp`

**Steps:**
1. Add failing integration-oriented pure assertions for `HH:MM:SS` segment composition, retained date, independent standby/runtime cache decisions, and prompt/rotation transitions.
2. Verify red.
3. Extract one renderer that accepts orientation, persona state, and explicit cache. Wire charging standby and active runtime to it. Keep compact pet clearing, same-size primary/dim time segments, date, and last-painted meters. Remove the now-unused full-screen runtime pet path without changing approval or functional screens.
4. Run focused tests, every firmware C++ test, `pio run -s`, and `git diff --check`.
5. Commit as `feat: share clock face across standby and runtime`.

