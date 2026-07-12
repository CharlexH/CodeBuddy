# Mac-Push Wi-Fi OTA Design

## Goal

After one final USB bootstrap, allow the user to run `code-buddy firmware update` on the paired Mac and install a firmware update on StickS3 without USB. The Mac initiates the operation, BLE coordinates it, Wi-Fi carries the application image, and the device independently authenticates, stages, verifies, boots, and either accepts or rolls back the image.

## Chosen approach

Use a short-lived HTTPS update server inside the existing `code-buddy` agent. This is preferable to BLE bulk transfer because the current NUS protocol has no resumable application acknowledgements and is slower for a roughly 2 MB image. It is preferable to a public update site for the first implementation because Code Buddy already depends on the paired Mac and no domain, cloud bucket, or CI secret is required.

USB remains the immutable recovery path. The currently connected device receives the OTA-capable bootstrap once over USB; all subsequent application updates use the Mac-local path.

## Trust and keys

Setup creates two independent P-256 key pairs outside the repository under `~/.code-buddy/ota/keys`, with directory mode `0700` and private-key mode `0600`:

- a local certificate authority used only to issue a short-lived HTTPS leaf certificate for the Mac's current LAN IP;
- a firmware-manifest signing key used to sign the exact canonical manifest bytes.

The bootstrap build embeds only the CA certificate and manifest public key through a generated, gitignored header. Private keys are never included in firmware, Git, command-line arguments, logs, bundles, or device storage. Losing the trust directory requires a USB trust bootstrap; rotating either key follows the same explicit recovery flow.

TLS authenticates the local Mac endpoint. The detached manifest signature authenticates the release independently of TLS. The signed manifest binds schema, monotonically increasing version, chip, image size, SHA-256 digest, and the one-time HTTPS image URL.

## User flow

1. In Settings, selecting Wi-Fi with no saved network starts a ten-minute WPA2 SoftAP provisioning session. The display shows the random AP password and local setup URL. A small captive portal accepts SSID/password from the Mac; the physical action and random AP password are the confidentiality boundary. Credentials are stored only in the ESP Wi-Fi/NVS namespace and can be forgotten on-device.
2. `code-buddy firmware update [firmware.bin]` selects or builds an app-only image, derives its version/size/SHA-256, creates canonical manifest bytes, and signs them.
3. The agent selects the active private LAN address, issues a short-lived certificate containing that IP in its SAN, binds a one-time HTTPS server, and creates an unguessable URL token.
4. The agent sends a small BLE `ota_offer` containing version, size, URL, and manifest signature metadata. No firmware bytes or Wi-Fi credentials travel over BLE.
5. The device displays the version and size. Only an A-button confirmation begins network or flash work; B cancels.
6. The device joins saved Wi-Fi, establishes TLS with the embedded CA, disables redirects, downloads bounded manifest/signature bytes, verifies the exact bytes before parsing, validates policy, and downloads the image.
7. The device writes only to `esp_ota_get_next_update_partition()`, requires exact byte count, calls `esp_ota_end()`, reads the written partition back, recomputes SHA-256, and calls `esp_ota_set_boot_partition()` only after a constant-time digest match.
8. On the first pending-verification boot, a supervisor confirms display, LittleFS, buttons, BLE advertising, and a running event loop within 30 seconds. Success calls `esp_ota_mark_app_valid_cancel_rollback()`. Failure or timeout calls `esp_ota_mark_app_invalid_rollback_and_reboot()` once, with bounded `esp_restart()` fallback.
9. The agent waits for BLE reconnection and the new firmware version, then stops the HTTPS server and reports success. Timeout or rejection reports the device's non-secret error and leaves the current firmware active.

## Transport and privacy boundaries

- BLE remains the Codex snapshot/approval control plane and carries only OTA coordination/status.
- Wi-Fi carries only signed release metadata and firmware bytes. No Codex account data, transcript, approval, or OpenAI credential is exposed over HTTP(S).
- The HTTPS server is one-shot, token-scoped, bounded to the selected artifact, and shuts down after success, failure, or timeout. It does not expose a directory listing or arbitrary file path.
- OTA is refused while an approval prompt is active, while the device lacks external power and has less than 50% battery, or when an image does not fit the inactive slot.
- Host BLE time sync must also call `settimeofday()` with UTC so TLS certificate validation has trustworthy system time. SNTP is a fallback only when the BLE time is absent or stale.

## Failure behavior

Network errors, TLS errors, malformed or unsigned manifests, version downgrade, size mismatch, write failure, readback mismatch, or cancellation never switch the boot slot. Interrupted downloads abort the staging handle. A pending image cannot remain running after an explicit health failure or deadline: it actively reboots into rollback.

The current known-good image and USB merged `full.bin` remain recovery artifacts. Secure Boot and flash encryption are deliberately not enabled in this change because their eFuse/key-custody migration is irreversible and changes the USB recovery boundary.

## Testing and acceptance

Pure host and firmware tests cover key permissions, canonical signing, one-shot server path policy, Wi-Fi/provisioning state, manifest byte-before-parse verification, downgrade and URL rejection, OTA call ordering, exact-size and readback digest gates, and the boot-health decision table.

Physical acceptance requires:

- USB flash of the OTA bootstrap;
- Wi-Fi provisioning, reconnect, forget, and reprovision without exposing credentials in logs;
- a complete Mac-initiated wireless update into the opposite slot;
- BLE reconnection showing the new firmware version;
- tampered signature, wrong digest, interrupted transfer, and forced health failure all preserving or restoring the prior bootable image;
- existing usage bars, pet-only runtime, rotation, BLE snapshots, and approval buttons still working.

