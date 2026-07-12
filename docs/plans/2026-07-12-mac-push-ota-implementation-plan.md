# Mac-Push Wi-Fi OTA Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Deliver a tested `code-buddy firmware update` flow that transfers a signed application image from the paired Mac to StickS3 over local HTTPS after one final USB bootstrap.

**Architecture:** The long-lived agent creates and signs a one-time release, serves it over an ephemeral HTTPS endpoint, and sends a BLE offer. StickS3 provisions Wi-Fi locally, authenticates the Mac and exact manifest bytes, writes only the inactive OTA slot, verifies the stored bytes, then confirms or rolls back the first boot.

**Tech Stack:** Python 3, `asyncio`, `ssl`, OpenSSL CLI, macOS launchd, ESP32-S3 Arduino core 2.0.17 / ESP-IDF 4.4.7, `WiFi`, `DNSServer`, `WebServer`, mbedTLS, `esp_ota_ops`, PlatformIO, native CoreBluetooth helper.

---

### Task 1: Add protected OTA trust and signed release tooling

**Files:**
- Create: `src/codex_buddy/ota_trust.py`
- Create: `src/codex_buddy/ota_release.py`
- Create: `scripts/generate-ota-trust.py`
- Create: `scripts/build-ota-release.py`
- Modify: `src/codex_buddy/runtime.py`
- Modify: `.gitignore`
- Test: `tests/test_ota_trust.py`
- Test: `tests/test_ota_release.py`

**Steps:**
1. Write failing tests for runtime paths, `0700`/`0600` permissions, idempotent key generation, exact canonical manifest bytes, P-256 signature verification, monotonic version binding, SHA-256/size binding, and refusal to place private material in a bundle.
2. Run the focused tests and record the expected missing-module/API failures.
3. Implement key generation through argument-safe `openssl` subprocess calls, protected paths under `~/.code-buddy/ota`, public-material export for the firmware build, canonical manifest creation, signing, and offline verification.
4. Run focused tests plus `git diff --check`.
5. Commit as `feat: add protected OTA release signing`.

### Task 2: Add the one-shot local HTTPS artifact server

**Files:**
- Create: `src/codex_buddy/ota_server.py`
- Modify: `src/codex_buddy/agent.py`
- Modify: `src/codex_buddy/runtime.py`
- Test: `tests/test_ota_server.py`
- Test: `tests/test_agent.py`

**Steps:**
1. Write failing tests for private-LAN address selection, IP-SAN leaf issuance, TLS-only serving, unguessable token paths, exact three-artifact allowlist, method rejection, redirect absence, byte/length stability, concurrency limit, timeout, and guaranteed teardown.
2. Verify red.
3. Implement an injected/testable one-shot HTTPS server. Add an agent-owned OTA session lock that suspends ordinary BLE snapshots only during offer/install coordination and always releases on failure.
4. Verify focused tests and the full host suite.
5. Commit as `feat: serve one-shot local OTA releases`.

### Task 3: Add physical Wi-Fi provisioning and trusted UTC

**Files:**
- Create: `firmware/src/wifi_state_logic.h`
- Create: `firmware/src/wifi_manager.h`
- Create: `firmware/src/wifi_manager.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/data.h`
- Modify: `firmware/src/settings_menu_logic.h`
- Modify: `firmware/src/stats.h`
- Test: `firmware/tests/wifi_state_logic_test.cpp`
- Test: `firmware/tests/settings_menu_logic_test.cpp`

**Steps:**
1. Write failing pure tests for unprovisioned/provisioning/connecting/online/error transitions, physical-only start, ten-minute expiry, cancel, forget, approval suspension, retry backoff, and TLS time freshness.
2. Verify red with the host C++ test command.
3. Implement a random-password WPA2 SoftAP and bounded captive portal using built-in libraries; redact credentials; store through ESP Wi-Fi/NVS; expose setup/status/forget UI; stop all provisioning services after success/cancel/timeout. Apply BLE UTC to `settimeofday()` and use bounded SNTP only when UTC is stale.
4. Run focused tests, all firmware C++ tests, and `pio run -s`.
5. Commit as `feat: add physical Wi-Fi provisioning`.

### Task 4: Authenticate Mac-local OTA offers and manifests

**Files:**
- Create: `firmware/src/ota_trust.h`
- Create: `firmware/src/ota_manifest_logic.h`
- Create: `firmware/src/ota_manifest.h`
- Create: `firmware/src/ota_manifest.cpp`
- Create: `firmware/scripts/inject-ota-trust.py`
- Modify: `firmware/platformio.ini`
- Modify: `firmware/src/data.h`
- Test: `firmware/tests/ota_manifest_logic_test.cpp`
- Test: `tests/test_ota_release.py`

**Steps:**
1. Generate fixture keys outside the repository and write failing tests for exact-byte signature verification, duplicate keys, UTF-8/BOM/NUL ambiguity, non-canonical JSON, unknown/retyped/missing fields, wrong chip, downgrade, invalid size/digest, public/non-private IP, non-HTTPS URL, redirect, and image path/token mismatch.
2. Verify red.
3. Implement build-time public trust injection that fails closed when OTA is enabled without trust material. Verify the detached ECDSA signature over raw manifest bytes before strict parsing and return one immutable descriptor.
4. Run firmware/host focused tests and `pio run -s` with generated public trust.
5. Commit as `feat: authenticate local OTA manifests`.

### Task 5: Stage, read back, and switch only a verified image

**Files:**
- Create: `firmware/src/ota_update_logic.h`
- Create: `firmware/src/ota_update.h`
- Create: `firmware/src/ota_update.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/data.h`
- Test: `firmware/tests/ota_update_logic_test.cpp`

**Steps:**
1. Write failing fake-adapter tests for confirmation, power gate, inactive-slot-only selection, exact size, Content-Length mismatch, interrupted download, TLS/redirect failure, write/end/read failure, constant-time readback mismatch, cancellation, and the sole successful order `begin -> write* -> end -> readback* -> digest_match -> set_boot`.
2. Verify red.
3. Implement bounded HTTPS download and OTA staging. Never call `set_boot_partition` on a failure path. Show progress without leaking URL/token and preserve approval priority.
4. Run focused/all firmware tests and `pio run -s`; inspect final slot headroom.
5. Commit as `feat: stage verified Mac-push OTA images`.

### Task 6: Confirm a healthy first boot or actively roll back

**Files:**
- Create: `firmware/src/ota_boot_health_logic.h`
- Create: `firmware/src/ota_boot_health.h`
- Create: `firmware/src/ota_boot_health.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/xfer.h`
- Test: `firmware/tests/ota_boot_health_logic_test.cpp`

**Steps:**
1. Write failing decision-table tests for non-pending boot, all required health bits, 30-second timeout, mark-valid once, explicit failure rollback once, returned-API restart fallback, and rollback-reason reporting without a loop.
2. Verify red.
3. Arm the supervisor before fallible setup, collect display/LittleFS/button/BLE/event-loop readiness, mark valid only when complete, and actively mark invalid/reboot on failure or timeout.
4. Run focused/all tests and build.
5. Commit as `feat: validate or roll back OTA boots`.

### Task 7: Add the Mac command and end-to-end coordination

**Files:**
- Modify: `src/codex_buddy/cli.py`
- Modify: `src/codex_buddy/agent.py`
- Modify: `src/codex_buddy/ble_transport.py`
- Modify: `src/codex_buddy/native_ble_helper/CodeBuddyBLEHelper.swift`
- Modify: `src/codex_buddy/setup_flow.py`
- Modify: `scripts/build-firmware-release.sh`
- Modify: `README.md`
- Modify: `firmware/REFERENCE.md`
- Test: `tests/test_cli.py`
- Test: `tests/test_agent.py`
- Test: `tests/test_ble_transport.py`

**Steps:**
1. Write failing tests for `code-buddy firmware update`, offer/accept/reject/status protocol, OTA-exclusive agent state, snapshot suspension, reconnect/version confirmation, timeout cleanup, and actionable errors.
2. Verify red.
3. Implement the CLI and agent orchestration. Default to the release app image, support an explicit image for development, never accept arbitrary device URLs, and wait for physical confirmation plus post-reboot BLE version proof.
4. Run the full host and firmware suites, native helper build, release build, and `git diff --check`.
5. Commit as `feat: push signed firmware from Mac`.

### Task 8: Physical bootstrap, wireless update, and fault proof

**Files:**
- Update only test evidence/docs if code changes are not required.

**Steps:**
1. Generate the real local trust directory outside Git and build bootstrap version N with its public material.
2. USB-flash version N to `/dev/cu.usbmodem1101`; capture boot, partition, rollback capability, BLE, and Wi-Fi provisioning evidence.
3. Provision the user's 2.4 GHz Wi-Fi without logging credentials.
4. Build/sign version N+1 and run `code-buddy firmware update`; capture offer confirmation, HTTPS transfer, inactive-slot digest, reboot, pending-health validation, BLE reconnect, reported version, and running snapshot.
5. Run negative physical checks: tampered signature/digest and interrupted transfer must not switch slots; a controlled pending-health failure must restore N without a loop.
6. Re-run 116+ host tests, every firmware C++ test, `pio run -s`, release verification, and worktree cleanliness. Dispatch final spec and quality reviews before reporting completion.

