# Secure Wi-Fi OTA Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an opt-in, signed, rollback-safe Wi-Fi OTA path for StickS3 while retaining BLE as the local approval/control transport.

**Architecture:** USB remains the bootstrap and recovery path. A physically initiated on-device provisioning flow stores Wi-Fi credentials locally, then a manual update command downloads only an application image to the inactive OTA slot over HTTPS. A signed manifest, pinned signing public key, image digest verification, boot-health confirmation, and rollback prevent an untrusted or broken image from becoming the active firmware.

**Tech Stack:** ESP32-S3, Arduino-ESP32/ESP-IDF APIs, `wifi_prov_mgr` Security 1, WPA2 SoftAP, `esp_https_ota`, `esp_ota_ops`, mbedTLS, NVS, PlatformIO, GitHub Release or equivalent HTTPS artifact host.

---

## Scope and non-goals

- Keep BLE/NUS as the only Codex approval and local bridge transport. Wi-Fi never receives Codex credentials, approval decisions, session data, or a raw firmware stream from the Mac.
- Wi-Fi is for optional on-device operations: OTA, a future NTP fallback, release notes, and diagnostics. The host-synced BLE clock remains the primary clock source.
- OTA updates only the application image. Do not OTA `full.bin`, bootloader, partition table, LittleFS, NVS, or an arbitrary URL.
- Do not run automatic background checks or automatic installs. The user must invoke both check and install on the device, and the update screen must show version, size, and release notes before download.
- Do not implement this without a real signing-key owner and a public HTTPS artifact location. A placeholder key, unsigned manifest, or a user-entered update URL is not an acceptable OTA design.

## Current readiness and decisions required

`firmware/platformio.ini` selects the ESP32 8 MB default partition layout, which provides `otadata` plus two application slots (`ota_0` and `ota_1`). The present USB release remains the merged `full.bin` flashed at `0x0`; the future OTA artifact is a separately built `firmware.bin` written only to the inactive application slot.

Before implementation, the release owner must provide:

1. A protected offline/private signing key and a rotation procedure; only the matching public verification key is compiled into firmware.
2. An HTTPS hostname under release-owner control for a versioned manifest and image (for example GitHub Releases behind a stable redirect, Cloudflare R2, or an equivalent immutable artifact host).
3. A release channel decision (`stable` first; `beta` only after stable works) and a minimum supported firmware version.
4. A security decision for a later irreversible hardening pass: ESP32-S3 Secure Boot v2 and flash encryption. This should be a separate, signed-off migration because enabling it changes USB recovery and key custody.

## Runtime state machine

```text
USB bootstrap
  -> Wi-Fi unprovisioned
  -> physical menu action: provisioning SoftAP
  -> credentials received / persisted
  -> connecting -> online
  -> physical menu action: check update
  -> manifest verified -> update available
  -> physical confirmation + power gate
  -> HTTPS download to inactive slot
  -> image digest verified -> set boot slot -> reboot
  -> pending verification boot
  -> hardware/FS/BLE health check
  -> mark valid OR rollback automatically
```

Failures always return to the normal BLE firmware. A failed network connection, manifest verification, digest check, or update download never changes the active boot partition.

## Provisioning and credential rules

- Provisioning starts only from a long physical button gesture in Settings; it expires after 10 minutes and is cancellable from the device.
- Run a named SoftAP with a randomly generated WPA2 password and ESP-IDF Wi-Fi Provisioning Security 1 proof-of-possession (PoP). Render the SSID, password/PoP, and QR code on the StickS3 screen.
- Use ESP-IDF `wifi_prov_mgr` rather than Arduino `WiFiProv` wrappers. The manager is destroyed after provisioning and logs must redact SSID, password, and PoP.
- Store credentials only in the ESP32 NVS provisioner namespace. Provide an on-device “Forget Wi-Fi” operation that erases them and returns to unprovisioned state.
- Never provision Wi-Fi over the existing NUS BLE service: its application protocol was built for buddy snapshots and approvals, not confidential credential entry.
- Join only the selected 2.4 GHz network. Use reconnect backoff with jitter and stop retries while an approval prompt is visible or a download is not explicitly requested.

## Manifest and update contract

The release pipeline publishes a canonical JSON manifest and detached signature. The device has one fixed production public key and accepts only an Ed25519 signature over the exact canonical manifest bytes (or an ESP-IDF-supported equivalent selected and tested before implementation).

```json
{
  "schema": 1,
  "channel": "stable",
  "version": "0.2.0",
  "chip": "esp32s3",
  "minimumVersion": "0.1.4",
  "publishedAt": "2026-07-11T09:00:00Z",
  "artifact": {
    "url": "https://updates.example.invalid/code-buddy/stable/0.2.0/firmware.bin",
    "sha256": "lowercase-64-hex-digest",
    "sizeBytes": 2015232
  },
  "releaseNotes": "Short device-safe release notes"
}
```

Validation order is: HTTPS certificate validation, manifest size/type/schema validation, signature validation, channel/chip/version policy, fixed-host URL policy, then image download. Stream the image to the inactive partition with `esp_https_ota`; calculate SHA-256 as it streams and compare it to the signed manifest before setting the new boot partition. Reject redirects to a different host, HTTP, oversized manifests/images, downgrades, and an image that does not fit the inactive slot.

## Boot health and rollback

- Enable and verify the ESP-IDF bootloader rollback configuration before exposing OTA in Settings.
- An updated image begins in a pending-verification state. In the first 30 seconds it must initialise NVS/LittleFS, display, buttons, BLE advertising, and its normal event loop without a reset or panic.
- Only after those checks call `esp_ota_mark_app_valid_cancel_rollback()`. A reset, watchdog, failed health check, or missed deadline leaves the image unconfirmed so the bootloader rolls back to the known-good slot.
- Require external power or battery at least 50% before download. Recheck immediately before setting the boot partition; refuse updates when charging/battery state is unavailable.
- Preserve a USB recovery guide and a Settings screen showing active version, pending version, rollback reason, and last update error code without secrets.

## Task 1: Inspect partition and bootloader capabilities

**Files:**
- Modify: `firmware/platformio.ini`
- Create: `firmware/tests/ota_partition_policy_test.cpp`
- Inspect: generated `.pio/build/m5stack-sticks3/partitions.bin`

**Step 1: Write failing policy tests**

Assert that the selected partition layout has `otadata`, two app slots of equal usable size, and no OTA code path can target bootloader/partition/LittleFS offsets.

**Step 2: Verify red**

Run: `cd firmware && pio test -e m5stack-sticks3 -f ota_partition_policy_test`

Expected: FAIL until the partition inspection/parser exists.

**Step 3: Implement minimal inspection and rollback configuration**

Add a checked-in partition CSV if the PlatformIO framework default cannot be reliably inspected. Configure the exact ESP-IDF rollback setting supported by the selected Arduino-ESP32 release; do not guess an sdkconfig flag.

**Step 4: Verify green and build**

Run: `cd firmware && pio run -s`

Expected: exit 0 and both OTA slots fit the current app with documented headroom.

**Step 5: Commit**

```bash
git add firmware/platformio.ini firmware/partitions firmware/tests/ota_partition_policy_test.cpp
git commit -m "feat: prepare rollback-capable OTA partitions"
```

## Task 2: Add secret-safe Wi-Fi provisioning state

**Files:**
- Create: `firmware/src/wifi_manager.h`
- Create: `firmware/src/wifi_manager.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/src/settings.h`
- Test: `firmware/tests/wifi_state_logic_test.cpp`

**Step 1: Write failing state-machine tests**

Cover unprovisioned → provisioning → saved → connecting → online, timeout/cancel, and forget-Wi-Fi. Assert that prompt/approval states suspend background reconnect work.

**Step 2: Verify red**

Run: `cd firmware && g++ -std=c++17 -Isrc tests/wifi_state_logic_test.cpp -o /tmp/wifi_state_logic_test && /tmp/wifi_state_logic_test`

Expected: FAIL because the pure transition helper does not exist.

**Step 3: Implement minimal manager**

Keep the state reducer independent of ESP headers. Use `wifi_prov_mgr` Security 1 in the ESP-specific adapter, generate per-session credentials from hardware RNG, redact logs, and stop/deinit the provisioning service on success, cancel, or timeout.

**Step 4: Verify green and hardware check**

Run unit test and `cd firmware && pio run -s`; on hardware prove the QR flow connects, persists after reboot, and forget removes the connection.

**Step 5: Commit**

```bash
git add firmware/src/wifi_manager.* firmware/src/main.cpp firmware/src/settings.h firmware/tests/wifi_state_logic_test.cpp
git commit -m "feat: add physical Wi-Fi provisioning"
```

## Task 3: Parse and authenticate update manifests

**Files:**
- Create: `firmware/src/ota_manifest.h`
- Create: `firmware/src/ota_manifest.cpp`
- Create: `firmware/src/ota_public_key.h`
- Test: `firmware/tests/ota_manifest_test.cpp`

**Step 1: Write failing parser/signature tests**

Use known-good signed test fixtures. Reject wrong key, modified byte, missing fields, wrong chip, invalid digest length, HTTP/foreign-host URL, oversized notes, unsupported schema, and semantic downgrade.

**Step 2: Verify red**

Run: `cd firmware && pio test -e m5stack-sticks3 -f ota_manifest_test`

Expected: fixtures fail authentication before verification code exists.

**Step 3: Implement minimal verifier**

Bound manifest input before parsing, canonicalise exactly once in the release tool and device implementation, and use the compiled public key. Keep the public key in source; keep the private key outside the repository and CI logs.

**Step 4: Verify green**

Run the test target plus an offline manifest verification fixture test on macOS.

**Step 5: Commit**

```bash
git add firmware/src/ota_manifest.* firmware/src/ota_public_key.h firmware/tests/ota_manifest_test.cpp
git commit -m "feat: verify signed OTA manifests"
```

## Task 4: Download, validate, and stage only the inactive app image

**Files:**
- Create: `firmware/src/ota_update.h`
- Create: `firmware/src/ota_update.cpp`
- Modify: `firmware/src/main.cpp`
- Test: `firmware/tests/ota_update_policy_test.cpp`

**Step 1: Write failing policy tests**

Cover power rejection, active-slot rejection, non-HTTPS rejection, SHA mismatch, interrupted download, image-size overflow, and only setting boot partition after all checks succeed.

**Step 2: Verify red**

Run the pure policy test with g++ before adapter code exists.

**Step 3: Implement minimal staging adapter**

Use `esp_https_ota` with normal TLS verification, no insecure certificate bypass, strict redirect policy, timeout/retry budget, streaming SHA-256, and the inactive application partition returned by `esp_ota_get_next_update_partition`. The UI shows phase and percent but never a URL or secret.

**Step 4: Verify green and fault injection**

On a sacrificial device/slot, interrupt network and power at each phase. Verify the original firmware always remains bootable and a bad digest never changes boot target.

**Step 5: Commit**

```bash
git add firmware/src/ota_update.* firmware/src/main.cpp firmware/tests/ota_update_policy_test.cpp
git commit -m "feat: stage verified OTA application images"
```

## Task 5: Confirm health or roll back

**Files:**
- Create: `firmware/src/ota_boot_health.h`
- Modify: `firmware/src/main.cpp`
- Test: `firmware/tests/ota_boot_health_test.cpp`

**Step 1: Write failing health-gate tests**

Require all boot conditions plus the deadline. Test that any failure or timeout does not mark valid.

**Step 2: Verify red**

Run the focused host C++ test; it must fail while no health aggregator exists.

**Step 3: Implement the minimal confirmation gate**

Check `esp_ota_get_state_partition`, gather display/buttons/LittleFS/BLE readiness, then mark valid exactly once. Surface a non-secret failure code and allow USB recovery.

**Step 4: Verify green on hardware**

Install a signed known-good image, then a deliberately boot-failing test image in the inactive slot. Prove first image becomes valid and the second rolls back automatically.

**Step 5: Commit**

```bash
git add firmware/src/ota_boot_health.h firmware/src/main.cpp firmware/tests/ota_boot_health_test.cpp
git commit -m "feat: confirm healthy OTA boots with rollback"
```

## Task 6: Add a signed release pipeline and offline USB recovery artifacts

**Files:**
- Modify: `scripts/build-firmware-release.sh`
- Create: `scripts/sign-ota-manifest.py`
- Create: `scripts/verify-ota-release.py`
- Modify: `.github/workflows/release.yml` (or the actual release workflow)
- Modify: `README.md`

**Step 1: Write a failing release verifier fixture**

The verifier must reject a manifest whose version, SHA-256, image size, or signature does not match the artifact.

**Step 2: Verify red**

Run `python3 scripts/verify-ota-release.py fixtures/bad-manifest.json`; expect a non-zero exit.

**Step 3: Implement the pipeline**

Build both `full.bin` (USB recovery) and app-only `firmware.bin` (OTA); produce SHA-256, sign the canonical manifest in a protected release environment, publish immutable versioned paths, and update only a signed stable-channel index. The private key never enters the firmware repository or a developer shell history.

**Step 4: Verify green**

Run local verification against a test signing key outside the repo, then run a release-candidate download/update/rollback test on physical hardware.

**Step 5: Commit**

```bash
git add scripts README.md .github/workflows
git commit -m "build: publish signed Code Buddy OTA releases"
```

## Task 7: Add future Wi-Fi features only behind the same privacy boundary

**Files:**
- Modify: `firmware/src/wifi_manager.*`
- Modify: `firmware/src/main.cpp`
- Test: `firmware/tests/wifi_state_logic_test.cpp`

After OTA is proven, add only these opt-in operations: NTP as an offline/host-absent clock fallback, Wi-Fi signal/status diagnostics, and signed release-note retrieval. Keep default telemetry off and leave Codex account limits, session content, and approval decisions on the existing local host/BLE design.

## Verification checklist

- Unit tests cover every state transition, signature rejection path, URL/power policy, digest mismatch, and boot-health decision.
- `pio run -s` succeeds with the selected Arduino-ESP32 version.
- First USB flash works exactly as today.
- Provision/cancel/forget have no credentials in serial log or device UI after completion.
- A successful signed OTA survives reboot and reports the new version.
- A tampered manifest, wrong-host URL, expired/invalid signature, bad image, network interruption, and forced boot failure all leave or restore a bootable known-good firmware.
- Existing BLE pairing, usage meter, approval buttons, and runtime landscape continue working before, during (where safe), and after an OTA attempt.
