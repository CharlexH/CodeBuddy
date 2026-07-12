# Secure Wi-Fi OTA Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an opt-in, signed, rollback-safe Wi-Fi OTA path for StickS3 while retaining BLE as the local approval/control transport.

**Architecture:** USB remains the bootstrap and recovery path. A physically initiated on-device provisioning flow stores Wi-Fi credentials locally, then a manual update command downloads only an application image to the inactive OTA slot from a dedicated same-origin HTTPS update host. A signature over the exact downloaded manifest bytes, readback digest verification of the written partition, boot-health confirmation, and active rollback keep an untrusted or broken image from becoming the lasting active firmware.

**Tech Stack:** ESP32-S3, Arduino-ESP32/ESP-IDF APIs, `wifi_prov_mgr` Security 1, WPA2 SoftAP, `esp_ota_ops`, `esp_partition`, mbedTLS, NVS, PlatformIO, and a release-owner-controlled HTTPS artifact origin.

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
2. A dedicated HTTPS origin under release-owner control for the versioned manifest, detached signature, and image (for example `https://updates.<release-owner-domain>` backed by Cloudflare R2 or equivalent immutable storage). GitHub Releases may mirror artifacts for humans, but device OTA must not use GitHub asset URLs or cross-domain redirects.
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
  -> write complete -> read back inactive slot -> digest verified
  -> set boot slot -> reboot
  -> pending verification boot
  -> hardware/FS/BLE health check
  -> mark valid OR mark invalid and actively reboot into rollback
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

The release pipeline publishes a canonical JSON manifest and detached signature from the same HTTPS origin as the image. The device has one fixed production public key and accepts only an Ed25519 signature over the exact downloaded manifest byte sequence (or an ESP-IDF-supported equivalent selected and tested before implementation). The signature is not over a parsed object, a reserialized object, or a digest supplied inside the manifest.

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

The manifest fetch order is deliberately byte-oriented:

1. Fetch the manifest and detached signature from fixed paths on the compiled production origin with normal TLS verification, redirects disabled, a strict response-size limit, and no content decoding or newline conversion.
2. Verify the detached signature over the exact downloaded manifest bytes **before** JSON parsing or field access.
3. Require strict UTF-8 with no BOM, NUL, invalid sequence, alternate character encoding, non-finite number, or non-canonical number/string escape. Run a duplicate-key-aware JSON validator at every object depth; a parser that silently keeps the first or last duplicate is not sufficient.
4. Parse only after steps 1–3 succeed, serialize the typed result with the same documented canonicalization profile used by the release tool, and require byte-for-byte equality with the signed bytes. This defends against parser/canonicalizer ambiguity in addition to the explicit duplicate-key check.
5. Validate the complete typed schema and policy: `schema`, `channel`, `version`, `chip`, `minimumVersion`, and `publishedAt`, then bind one artifact tuple consisting of `artifact.url`, lowercase 64-hex `artifact.sha256`, integer `artifact.sizeBytes`, manifest `version`, and `channel`. No field may have an alias, implicit default, lossy numeric conversion, or unconsumed duplicate/unknown replacement.

The compiled update origin is one exact `https` scheme, lowercase host, and port. The manifest, signature, and artifact URL must use that same origin; redirects are disabled rather than followed. Paths must be versioned, immutable, normalized, free of user info/fragments/dot segments, and match `/code-buddy/<channel>/<version>/firmware.bin`. Reject HTTP, a foreign host or port, any redirect, oversized manifests/images, downgrades, a response length different from signed `sizeBytes`, and an image that does not fit the inactive slot. GitHub Releases remains an optional mirror only because its asset redirects do not satisfy this fixed-origin device policy.

Do not use the convenience `esp_https_ota_finish()` path for the security decision because its finish/validation ordering does not expose the exact written partition for the required pre-switch digest gate. Use `esp_ota_begin()` + bounded `esp_ota_write()` calls to the inactive application partition, tracking received bytes only for progress. After the final byte, require received length == signed `sizeBytes`, call `esp_ota_end()` to close and validate the image, then read exactly `sizeBytes` back from that partition through `esp_partition_read()` in bounded chunks and compute SHA-256 over the readback bytes. Compare that digest in constant time with the signed lowercase digest. Only after both `esp_ota_end()` and readback digest verification succeed may code call `esp_ota_set_boot_partition()`. Erase/restart staging on any error; a streaming download hash may be retained as diagnostics, but it must not replace partition readback and must never authorize a boot-slot switch.

## Boot health and rollback

- Enable and verify the ESP-IDF bootloader rollback configuration before exposing OTA in Settings.
- An updated image begins in a pending-verification state. In the first 30 seconds it must initialise NVS/LittleFS, display, buttons, BLE advertising, and its normal event loop without a reset or panic.
- Only after all checks call `esp_ota_mark_app_valid_cancel_rollback()` exactly once. Any explicit health failure or deadline expiry must call `esp_ota_mark_app_invalid_rollback_and_reboot()`; merely returning to the event loop or leaving the image unconfirmed is not sufficient because rollback should not depend on a later incidental reset.
- Arm the 30-second deadline before fallible application initialisation and service it from a minimal boot-health supervisor that does not depend on Wi-Fi, BLE, LittleFS, or the normal UI task. Its failure path is one-shot: atomically latch `rollback_requested`, persist only a non-secret bounded reason when NVS is usable, stop normal startup, call the invalid/rollback/reboot API, and never retry it in a loop. If the API unexpectedly returns, log only the error code, wait a bounded interval, and call `esp_restart()`; the still-pending bootloader state must then select the prior valid slot. OTA must remain hidden unless bootloader rollback is proven enabled, preventing a reboot loop on a configuration without rollback.
- Track pending-boot attempts and the last rollback reason independently from update availability. The known-good image clears the attempt marker after observing a rollback. Tests must prove that a health timeout causes an active reboot request once, that a reboot while still pending cannot re-enter OTA download, and that repeated failures land on the prior valid image rather than alternating slots or looping indefinitely.
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

**Step 1: Write failing byte-authentication and parser tests**

Use known-good signed byte fixtures. Prove verification consumes the original byte buffer before parsing. Reject wrong key, any modified whitespace/escape/newline byte, UTF-8 BOM, invalid UTF-8, NUL, duplicate keys at root and nested object levels, non-canonical equivalent JSON, missing/unknown/retyped fields, wrong chip, invalid digest length/case, size overflow, artifact version/path mismatch, HTTP/foreign-origin/alternate-port URL, redirect response, oversized notes, unsupported schema, and semantic downgrade.

**Step 2: Verify red**

Run: `cd firmware && pio test -e m5stack-sticks3 -f ota_manifest_test`

Expected: fixtures fail authentication before verification code exists.

**Step 3: Implement minimal byte-first verifier**

Fetch bounded raw bytes and the detached signature from fixed same-origin paths. Verify the signature over those bytes before invoking JSON code. Then run a strict UTF-8/canonical JSON scanner with explicit duplicate-key rejection, parse into fixed-width typed fields, reserialize canonically, and compare it byte-for-byte with the signed input. Validate and return one immutable artifact descriptor containing version/channel plus the exact URL, digest, and size; the downloader accepts only this descriptor, never independently parsed fields. Keep the public key in source; keep the private key outside the repository and CI logs.

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

**Step 1: Write failing policy and ordering tests**

Cover power rejection, active-slot rejection, non-HTTPS/foreign-origin/redirect rejection, signed-size versus `Content-Length` and received-length mismatch, interrupted download, image-size overflow, `esp_ota_end()` failure, partition-read failure, and readback SHA mismatch. Use a fake adapter/event log to assert the only successful order is `begin -> write* -> end -> partition_read* -> digest_match -> set_boot_partition`, and that no failure path emits `set_boot_partition`.

**Step 2: Verify red**

Run the pure policy test with g++ before adapter code exists.

**Step 3: Implement minimal staging adapter**

Use the ordinary ESP HTTPS client with normal TLS verification, no insecure certificate bypass, redirects disabled, and a bounded timeout/retry budget. Resolve the inactive application partition with `esp_ota_get_next_update_partition()`, then stage through `esp_ota_begin()`/`esp_ota_write()`/`esp_ota_end()`. Require the exact signed byte count, read the written partition back with `esp_partition_read()`, hash the readback, compare it in constant time, and only then call `esp_ota_set_boot_partition()`. The UI shows phase and percent but never a URL or secret.

**Step 4: Verify green and fault injection**

On a sacrificial device/slot, interrupt network and power during erase/write, after `esp_ota_end()`, during partition readback, and immediately before/after the boot-slot switch. Inject a transport hash that matches while corrupting mocked/readback partition bytes to prove only the readback digest authorizes the switch. Verify the original firmware always remains bootable and a bad digest or read failure never changes the boot target.

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

Require all boot conditions plus the deadline. Test that success marks valid once; any explicit failure or timeout latches rollback once, calls `esp_ota_mark_app_invalid_rollback_and_reboot()`, and cannot resume normal startup. Test the API-unexpectedly-returned fallback invokes one bounded `esp_restart()`, and that a second pending boot selects the prior valid partition rather than looping.

**Step 2: Verify red**

Run the focused host C++ test; it must fail while no health aggregator exists.

**Step 3: Implement the minimal confirmation gate**

Arm an independent deadline before fallible startup, check `esp_ota_get_state_partition`, and gather display/buttons/LittleFS/BLE readiness without making the supervisor depend on those subsystems. Mark valid exactly once on success. On failure/timeout, atomically stop startup and actively mark invalid/rollback/reboot once, with bounded restart fallback if the API returns. Surface a non-secret failure code from the recovered known-good image and retain USB recovery.

**Step 4: Verify green on hardware**

Install a signed known-good image, then images that fail immediately, fail one required subsystem, and hang past the deadline. Prove the good image becomes valid, each bad image actively reboots, the bootloader selects the prior valid image, the rollback reason is visible there, and no case produces repeated pending-image boots or a reboot loop.

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

The verifier must reject a manifest whose version/path, SHA-256, image size, origin, canonical bytes, or signature does not match the artifact. Include duplicate-key, alternate-encoding, redirect, and cross-origin fixtures.

**Step 2: Verify red**

Run `python3 scripts/verify-ota-release.py fixtures/bad-manifest.json`; expect a non-zero exit.

**Step 3: Implement the pipeline**

Build both `full.bin` (USB recovery) and app-only `firmware.bin` (OTA); derive version, exact byte size, and SHA-256 from the final app artifact; construct one canonical manifest whose versioned same-origin URL binds those values; sign the exact emitted bytes in a protected release environment; and verify them again before upload. Publish manifest, detached signature, and image to immutable paths on the dedicated update origin, then atomically update only a signed stable-channel pointer on that origin. Configure the device-facing origin to serve bytes without content transformation and to reject redirects; GitHub Releases may receive a mirror but is never written into the device manifest. The private key never enters the firmware repository or a developer shell history.

**Step 4: Verify green**

Run local verification against a test signing key outside the repo. As a release gate, fetch every production URL exactly as the device does and prove: valid public CA chain and hostname, exact expected origin/port, zero redirects, immutable versioned cache policy, manifest/signature byte stability, advertised size, artifact digest, and signature all match. Then run a release-candidate download/readback/update/rollback test on physical hardware. Do not enable the Settings OTA entry until the dedicated origin and these gates pass in production.

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

- Unit tests cover every state transition, byte-before-parse signature rejection path, duplicate-key/encoding ambiguity, same-origin/no-redirect URL policy, partition-readback digest mismatch, operation ordering, and active boot-health rollback decision.
- `pio run -s` succeeds with the selected Arduino-ESP32 version.
- First USB flash works exactly as today.
- Provision/cancel/forget have no credentials in serial log or device UI after completion.
- A successful signed OTA survives reboot and reports the new version.
- A tampered or non-canonical manifest, duplicate JSON key, ambiguous encoding, wrong-origin URL, any redirect, invalid signature, signed-size mismatch, corrupt written partition, network interruption, and forced/timeout boot failure all leave or actively restore a bootable known-good firmware.
- Production rollout is blocked until the dedicated update origin passes TLS, no-redirect, immutable-path, exact-byte, signature, size, digest, readback, and rollback-loop gates on physical hardware.
- Existing BLE pairing, usage meter, approval buttons, and runtime landscape continue working before, during (where safe), and after an OTA attempt.
