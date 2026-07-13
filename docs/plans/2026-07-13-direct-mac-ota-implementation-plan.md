# Direct Mac OTA Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Let a physically authorized StickS3 install an explicitly requested, signed Mac firmware update without device navigation or per-update A confirmation.

**Architecture:** The host signs a short-lived, device-bound OTA authorization envelope with the USB-bootstrap manifest key. Compatible firmware verifies that envelope before UI/network work, then follows either persisted `Ask` or `Direct` policy while retaining the existing signed manifest, pinned HTTPS, inactive-slot readback, boot-health, and rollback pipeline. Legacy six-field offers remain available only for bootstrapping pre-Direct firmware.

**Tech Stack:** Python 3.9/3.13, asyncio Unix sockets, ECDSA P-256/OpenSSL, Arduino C++17, ArduinoJson, mbedTLS, ESP32 Preferences/NVS, M5Unified, PlatformIO, pytest.

---

### Task 1: Host signed authorization envelope and legacy negotiation

**Files:**
- Modify: `src/codex_buddy/ota_protocol.py`
- Modify: `src/codex_buddy/ota_coordination.py`
- Test: `tests/test_ota_protocol.py`
- Test: `tests/test_ota_coordination.py`

**Step 1: Write failing host protocol tests**

Add tests proving canonical bytes are deterministic and domain-separated; the signed offer contains exactly ten bounded fields; invalid device names/times fail; signature verification succeeds; and firmware `<0.1.6` still receives the legacy six-field offer.

**Step 2: Verify RED**

Run: `PYTHONPATH=src .venv/bin/pytest -q tests/test_ota_protocol.py tests/test_ota_coordination.py`

Expected: FAIL because signed authorization helpers and version negotiation do not exist.

**Step 3: Implement the minimum host behavior**

Add `canonical_ota_authorization_bytes(...)` and `build_signed_ota_offer(...)`. Canonical fields are `action`, `device`, `expiresAt`, `generation`, `issuedAt`, `manifestUrl`, `nonce`, `signatureUrl`, `sizeBytes`, and `version`; `action` is `code-buddy-firmware-install-v1`. DER signatures are lowercase hex. Device names must match `Codex-[0-9A-F]{4}` and expiry must be short and forward-moving.

In the coordinator, read the BLE transport device name, use the injected clock, and send the signed offer only when the probed current firmware is at least `0.1.6`; otherwise send the unchanged legacy offer.

**Step 4: Verify GREEN**

Run the same pytest command and require zero failures.

**Step 5: Commit**

```bash
git add src/codex_buddy/ota_protocol.py src/codex_buddy/ota_coordination.py tests/test_ota_protocol.py tests/test_ota_coordination.py
git commit -m "feat: sign direct OTA authorizations"
```

### Task 2: Device verifies signed offers before network or UI

**Files:**
- Create: `firmware/src/ota_authorization_logic.h`
- Create: `firmware/tests/ota_authorization_logic_test.cpp`
- Modify: `firmware/src/ota_manifest.h`
- Modify: `firmware/src/ota_manifest.cpp`
- Modify: `firmware/src/ota_manifest_logic.h`
- Modify: `firmware/src/data.h`
- Modify: `firmware/src/ota_status.cpp`
- Test: `firmware/tests/ota_manifest_logic_test.cpp`
- Test: `firmware/tests/ota_status_logic_test.cpp`

**Step 1: Write failing native tests**

Cover canonical reconstruction, DER-hex bounds/decoding, good signature callback, bad signature, wrong `Codex-XXXX`, expired/future envelope, invalid time ordering, replayed nonce/generation, malformed fields, and legacy offers requiring a physical receive window.

**Step 2: Verify RED**

Run each new/changed native test with:

```bash
c++ -std=c++17 -Ifirmware/src firmware/tests/ota_authorization_logic_test.cpp -o /tmp/ota_authorization_logic_test && /tmp/ota_authorization_logic_test
```

Expected: compile/test failure because the authorization API is missing.

**Step 3: Implement verification**

Expose a generic detached P-256/SHA-256 verifier from `ota_manifest.cpp`. Parse only the exact signed offer shape. Reconstruct canonical bytes into a bounded buffer, validate the current device name derived from the BT MAC, trusted epoch, expiry, semver/size/URLs, and recent replay identity, then verify the DER signature before calling the existing bounded offer acceptance logic.

Unsigned legacy offers may only use an already-open physical receive window. Invalid signed offers must be rejected before opening a receive window or starting OTA.

**Step 4: Verify GREEN**

Compile and run the new test plus `ota_manifest_logic_test.cpp` and `ota_status_logic_test.cpp`; require zero failures.

**Step 5: Commit**

```bash
git add firmware/src/ota_authorization_logic.h firmware/src/ota_manifest.h firmware/src/ota_manifest.cpp firmware/src/ota_manifest_logic.h firmware/src/data.h firmware/src/ota_status.cpp firmware/tests/ota_authorization_logic_test.cpp firmware/tests/ota_manifest_logic_test.cpp firmware/tests/ota_status_logic_test.cpp
git commit -m "feat: verify signed OTA offers on device"
```

### Task 3: Persisted Ask/Direct policy and device UX

**Files:**
- Modify: `firmware/src/stats.h`
- Modify: `firmware/src/settings_menu_logic.h`
- Modify: `firmware/src/ota_update.h`
- Modify: `firmware/src/ota_update.cpp`
- Modify: `firmware/src/main.cpp`
- Test: `firmware/tests/settings_menu_logic_test.cpp`
- Test: `firmware/tests/ota_update_logic_test.cpp`
- Test: `firmware/tests/ota_ui_logic_test.cpp`

**Step 1: Write failing policy/UI tests**

Prove the new `auto ota` row exists before manual `ota update`, default policy is Ask, physical toggle persists, Direct auto-authorizes only a verified signed offer, Ask reaches A/B without prior settings navigation, auto progress shows no Install hint, B remains available until boot commit, and factory reset clears policy via the existing namespace wipe.

**Step 2: Verify RED**

Compile/run the three native tests and require the new assertions to fail for missing behavior.

**Step 3: Implement policy and UX**

Add `Settings::autoOta`, NVS key `s_aota`, default false. Add the `auto ota` row while retaining `ota update` as recovery/manual receive. Carry a verified-offer `automatic` bit into OTA runtime/view. Direct consumes the offer without A; Ask automatically shows the confirmation card. Both wake the screen and show target metadata/progress. B/CLI cancellation and all existing functional/power/Wi-Fi gates remain unchanged.

**Step 4: Verify GREEN**

Compile/run the tests again, then compile and run all `firmware/tests/*_test.cpp`.

**Step 5: Commit**

```bash
git add firmware/src/stats.h firmware/src/settings_menu_logic.h firmware/src/ota_update.h firmware/src/ota_update.cpp firmware/src/main.cpp firmware/tests/settings_menu_logic_test.cpp firmware/tests/ota_update_logic_test.cpp firmware/tests/ota_ui_logic_test.cpp
git commit -m "feat: add direct Mac OTA policy"
```

### Task 4: Harden local control-plane permissions

**Files:**
- Modify: `src/codex_buddy/setup_flow.py`
- Modify: `src/codex_buddy/agent.py`
- Modify: `src/codex_buddy/agent_runtime.py`
- Test: `tests/test_setup_flow.py`
- Test: `tests/test_agent.py`
- Test: `tests/test_agent_runtime.py`

**Step 1: Write failing permission tests**

Prove existing runtime roots are corrected to `0700`, the bound agent socket is `0600`, macOS peer credentials with another uid are rejected before command parsing, and same-uid clients continue to work. Non-macOS behavior must stay testable and fail closed only when peer identity is available but invalid.

**Step 2: Verify RED**

Run: `PYTHONPATH=src .venv/bin/pytest -q tests/test_setup_flow.py tests/test_agent.py tests/test_agent_runtime.py`

Expected: FAIL on current `0755` runtime/socket behavior and absent peer check.

**Step 3: Implement hardening**

Use real-directory checks before chmod, enforce root `0700`, chmod the Unix socket to `0600` immediately after bind, and validate Darwin `LOCAL_PEERCRED` uid against `os.geteuid()` before reading a request. Do not expose credentials in errors/logs.

**Step 4: Verify GREEN and live permissions**

Run the focused tests and require zero failures. After reinstall/restart, verify `stat` reports `700` for `~/.code-buddy` and `600` for `agent.sock`.

**Step 5: Commit**

```bash
git add src/codex_buddy/setup_flow.py src/codex_buddy/agent.py src/codex_buddy/agent_runtime.py tests/test_setup_flow.py tests/test_agent.py tests/test_agent_runtime.py
git commit -m "fix: restrict OTA control plane to local user"
```

### Task 5: Versioned release, reviews, and two-stage device proof

**Files:**
- Modify: `pyproject.toml`
- Modify: `src/codex_buddy/__init__.py`
- Modify: `firmware/src/firmware_version.h`
- Modify: `src/codex_buddy/firmware/code-buddy-sticks3-app.bin`

**Step 1: Run complete verification before release**

Run Python tests on supported 3.9 and 3.13 environments, all native C++ tests, `pio run`, rollback symbol verification, fresh wheel install, strict helper codesign, and signed release inspection. Require zero failures aside from the documented websockets deprecation warnings.

**Step 2: Build and install `0.1.6`**

Bump all three version sources to `0.1.6`, build release artifacts, install the new host wheel, restart the agent, and use the legacy manual OTA path from device `0.1.5`. Require CLI and serial proof of `version=0.1.6`, `health=valid`.

**Step 3: Physically enable Direct once**

On device Settings, switch `auto ota` to `on`. No secret is entered or moved.

**Step 4: Prove no-touch Direct OTA with `0.1.7`**

Bump/build/install `0.1.7`, run `code-buddy firmware update` while the device is on an ordinary screen, and do not navigate or press A. Require signed-offer acceptance, download/readback, reboot, BLE reconnect, and exact `version=0.1.7`, `health=valid`.

**Step 5: Final review and commit**

Run a spec compliance review, code quality review, complete tests/build again, commit versioned artifacts, confirm a clean worktree, and do not push the remote branch.
