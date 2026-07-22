# Token Heartbeat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the latest 20 seconds of input-plus-output token consumption as a smooth 64-sample landscape heartbeat.

**Architecture:** A host-side `TokenHeartbeat` owns per-session baselines and 64 rolling 312.5ms bins, then emits a compact validated string in each snapshot. Firmware decodes the window atomically, ages it locally, and renders an upward-only anti-aliased green-to-mint curve with fallback to `activity20`.

**Tech Stack:** Python 3.9+, pytest, BLE newline JSON, ArduinoJson 7, C++17 native firmware tests, LovyanGFX/M5Unified.

## Global Constraints

- Keep the graph in the existing 64-by-14-pixel region.
- Preserve a fixed 20-second window and fixed logarithmic scale.
- Use 64 samples and 20/60/20 smoothing with total weight one.
- Do not emit historical spikes on first observation, reset, compaction, or disappearing sessions.
- Preserve `activity20` fallback for old hosts.

---

### Task 1: Host token window

**Files:**
- Create: `src/codex_buddy/token_heartbeat.py`
- Create: `tests/test_token_heartbeat.py`

**Interfaces:**
- Produces: `TokenHeartbeat.observe(session_id: str, total_tokens: int, now: float) -> None`
- Produces: `TokenHeartbeat.encoded(now: float) -> str`, a versioned 64-byte intensity payload encoded with URL-safe base64 without padding.

- [ ] **Step 1: Write failing tests** for first-observation baseline, positive deltas, counter decrease, concurrent sessions, 20-second aging, 20/60/20 conservation, saturating arithmetic, fixed `log1p` mapping, and exact encoding length/round-trip.
- [ ] **Step 2: Run** `PYTHONPATH=src .venv/bin/python -m pytest -q tests/test_token_heartbeat.py` and confirm failures are caused by the missing module.
- [ ] **Step 3: Implement** a 64-bin rolling window with `BIN_SECONDS = 20.0 / 64.0`, per-session baselines, integer raw bins, `log1p(value) / log1p(32000)` mapping to `0...255`, and strict URL-safe base64 encoding.
- [ ] **Step 4: Run the focused tests** and confirm all pass.

### Task 2: Agent integration and total token semantics

**Files:**
- Modify: `src/codex_buddy/agent.py`
- Modify: `src/codex_buddy/reducer.py`
- Modify: `src/codex_buddy/proxy.py`
- Modify: `tests/test_agent.py`
- Modify: `tests/test_reducer.py`
- Modify: `tests/test_bridge.py`

**Interfaces:**
- Consumes: `TokenHeartbeat.observe` and `.encoded` from Task 1.
- Produces: optional BLE snapshot field `token20v1: str`.

- [ ] **Step 1: Add failing tests** proving managed and read-only per-session totals feed the window, snapshots include `token20v1`, input plus output are used when provided, first observation is quiet, and decreasing counters rebaseline.
- [ ] **Step 2: Run** the focused agent/reducer/proxy tests and confirm expected failures.
- [ ] **Step 3: Integrate** one `TokenHeartbeat` into `BuddyAgent`, observe every current session record before snapshot creation, prune absent session baselines, and add `token20v1` to `BuddySnapshot.as_ble_payload()` only when available. Parse managed usage as total tokens when supplied, otherwise sum input and output counters, retaining safe fallback keys.
- [ ] **Step 4: Run** the focused tests and confirm all pass.

### Task 3: Firmware transport and decoding

**Files:**
- Create: `firmware/src/token_heartbeat_logic.h`
- Create: `firmware/tests/token_heartbeat_logic_test.cpp`
- Modify: `firmware/src/data.h`
- Modify: existing JSON state tests under `firmware/tests/`

**Interfaces:**
- Produces: `TokenHeartbeatState` containing 64 intensities, receive time, and validity.
- Produces: strict decoder for `token20v1` and local aging helpers.

- [ ] **Step 1: Add failing native tests** for exact decoding, invalid alphabet/length atomic rejection, local aging, disconnect aging, intensity-to-height bounds, and old-host invalid state.
- [ ] **Step 2: Run** `clang++ -std=c++17 -Ifirmware/src firmware/tests/token_heartbeat_logic_test.cpp -o /tmp/token_heartbeat_logic_test && /tmp/token_heartbeat_logic_test` and confirm failure.
- [ ] **Step 3: Implement** allocation-free URL-safe base64 decoding, strict 64-byte output, saturating elapsed-bin shifts, and atomic assignment in `data.h`.
- [ ] **Step 4: Run** the focused native and JSON tests and confirm all pass.

### Task 4: Smooth firmware curve and fallback

**Files:**
- Modify: `firmware/src/landscape_dashboard_logic.h`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/tests/landscape_dashboard_logic_test.cpp`

**Interfaces:**
- Consumes: valid token samples from Task 3.
- Produces: token curve rendering with `activity20` fallback.

- [ ] **Step 1: Add failing tests** proving 0 maps to the centerline, 255 maps to maximum upward amplitude, intermediate values are monotonic, all points remain inside the 64-by-14 region, scrolling is right-to-left, strong values move from green toward mint, and invalid token state uses `activity20`.
- [ ] **Step 2: Run** the landscape native test and confirm expected failures.
- [ ] **Step 3: Implement** per-pixel 64-sample curve generation, Catmull-Rom or monotone cubic interpolation without negative peaks, intensity-dependent RGB565 color blending, and off-screen atomic presentation. Retain the existing activity renderer only as fallback.
- [ ] **Step 4: Run** the focused native tests and confirm all pass.

### Task 5: Release verification

**Files:**
- Modify: `CHANGELOG.md`, `README.md`, `README.zh-CN.md`, `firmware/README.md`
- Modify: version declarations and packaged application firmware.

- [ ] **Step 1: Run** every tracked host test and every tracked firmware-native test.
- [ ] **Step 2: Build** with `./scripts/build-firmware-release.sh` and inspect the embedded version.
- [ ] **Step 3: OTA** the `*-app.bin` image and require `running`, `100%`, and `health: valid` from fresh device state.
