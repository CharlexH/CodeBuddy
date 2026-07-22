# Orientation Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent startup and page-return flashes through portrait while preserving normal auto-rotation hysteresis.

**Architecture:** Extend the shared orientation policy with an explicit initial resolver and remembered stable orientation. Auto-oriented surfaces resolve a strong IMU pose before their first draw; portrait-only surfaces may rotate the LCD but never destroy the remembered home orientation.

**Tech Stack:** C++17 pure logic tests, StickS3 IMU, M5Unified/LovyanGFX.

## Global Constraints

- Menus, settings, Wi-Fi, reset, and full OTA pages remain portrait-only.
- Strong sideways first samples select rotation 1 or 3 before any home frame is drawn.
- Ambiguous cold-start samples draw no speculative layout.
- Existing transition and left/right swap hysteresis remains after initialization.
- Forced portrait and forced landscape resolve immediately.

---

### Task 1: Initial orientation policy

**Files:**
- Modify: `firmware/src/clock_orient_logic.h`
- Modify: `firmware/tests/clock_orient_logic_test.cpp`
- Modify: `firmware/tests/screen_orient_logic_test.cpp`

**Interfaces:**
- Produces: `ClockOrientationState` with orientation, stable orientation, counters, and resolved flag.
- Produces: `clockOrientResolveInitialForStickS3(...)` and update helpers.

- [ ] **Step 1: Add failing tests** for strong portrait, strong landscape 1/3, ambiguous cold start, remembered-orientation fallback, forced settings, and unchanged post-resolution hysteresis.
- [ ] **Step 2: Run** the two focused native tests and confirm failures.
- [ ] **Step 3: Implement** strong-pose classification using the existing axis thresholds, explicit unresolved state, stable-orientation memory, and compatibility wrappers only where needed.
- [ ] **Step 4: Run** the focused tests and confirm all pass.

### Task 2: Render gate and transition integration

**Files:**
- Modify: `firmware/src/screen_orient_logic.h`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/tests/screen_orient_logic_test.cpp`
- Modify: `firmware/tests/clock_display_logic_test.cpp`

**Interfaces:**
- Consumes: orientation state from Task 1.
- Produces: a render decision that suppresses auto-surface drawing until resolved.

- [ ] **Step 1: Add failing tests** for cold sideways startup, menu/settings/Wi-Fi/reset/OTA return, standby-to-active transitions, approval entry/exit, ambiguous IMU, and the invariant that no portrait frame precedes a clearly sideways home frame.
- [ ] **Step 2: Run** focused tests and confirm failures.
- [ ] **Step 3: Replace** unconditional orientation resets with surface eligibility transitions. Keep portrait LCD rotation local to portrait-only pages, retain stable auto orientation, resolve current pose before the first eligible draw, and skip rendering while unresolved.
- [ ] **Step 4: Audit** every `setRotation(0)` call so it either belongs to a portrait-only draw or restores a temporary direct draw without clearing auto-orientation state.
- [ ] **Step 5: Run** focused tests and confirm all pass.

### Task 3: Device verification

**Files:**
- Modify: release documentation/version files together with the token-heartbeat release.

- [ ] **Step 1: Run** all firmware-native and host tests.
- [ ] **Step 2: Build** the release firmware and verify flash/RAM limits.
- [ ] **Step 3: OTA** and require fresh `running / valid / 100%` readback.
- [ ] **Step 4: Physically verify** cold sideways boot and returns from portrait-only pages do not flash a portrait home frame.
