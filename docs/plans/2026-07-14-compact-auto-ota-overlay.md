# Compact Auto OTA Overlay Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Show a minimal orientation-aware progress overlay for Direct OTA while preserving the existing Ask confirmation window.

**Architecture:** Extend the pure OTA UI policy with compact-overlay selection and deterministic portrait/landscape geometry. Track automatic overlay state separately in `main.cpp`, keep shared-face orientation eligible, and render the compact overlay after the underlying portrait sprite or landscape LCD surface.

**Tech Stack:** C++17 pure firmware tests, Arduino/M5Unified canvas APIs, PlatformIO ESP32-S3 build, Code Buddy signed OTA.

---

### Task 1: Add compact overlay policy and geometry

**Files:**
- Modify: `firmware/src/ota_ui_logic.h`
- Test: `firmware/tests/ota_ui_logic_test.cpp`

1. Add failing assertions that Direct selects a compact overlay, Ask does not, and portrait/landscape rectangles are 119x52 at (8,94) and 160x44 at (40,45).
2. Compile and run the native test; expect failure.
3. Add `compactOverlay` to `OtaUiPlan` plus `OtaCompactOverlayLayout` and `otaCompactOverlayLayout(bool)`.
4. Re-run the native test; expect pass.

### Task 2: Separate Direct overlay state from the Ask window

**Files:**
- Modify: `firmware/src/main.cpp`

1. Add `otaCompactOverlay` state and select it only for visible automatic updates.
2. Keep `otaReceiveScreen` for nonautomatic confirmation and manual receive only.
3. Exclude only the full window, not the compact overlay, from shared clock/runtime selection.
4. Preserve B cancellation while the compact overlay is cancellable.

### Task 3: Render compact progress in both orientations

**Files:**
- Modify: `firmware/src/main.cpp`

1. Add a shared canvas renderer for version, phase, percentage, and progress bar.
2. Draw it before portrait sprite pushes and after landscape direct-LCD draws.
3. Run OTA UI and shared clock native tests; expect pass.
4. Run `pio run`; expect success.

### Task 4: Package and deploy

**Files:**
- Modify: `src/codex_buddy/firmware/code-buddy-sticks3-app.bin`

1. Run `scripts/build-firmware-release.sh` and verify embedded version 0.1.8.
2. Commit the implementation and packaged application image.
3. Reinstall Code Buddy 0.1.8 locally if the packaged image changed.
4. OTA the device and verify `running`, version `0.1.8`, health `valid`.
