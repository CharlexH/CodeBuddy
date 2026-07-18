# Changelog

## 0.1.17 - 2026-07-19

- Lifted and vertically centered the landscape pet and task-status regions, enlarged the pet viewport, and increased GIF pet rendering from 50% to 60% scale.

## 0.1.16 - 2026-07-19

- Centered the stacked landscape month and day within their right-aligned block and tightened the minute-to-second spacing.

## 0.1.15 - 2026-07-19

- Reordered the landscape dashboard so the clock and date sit at the top, with the pet and task-status columns centered beneath them.

## 0.1.14 - 2026-07-19

- Moved the landscape `RUN` / `ASK` / `NEW` dashboard down four pixels.
- Reduced the landscape `HH:MM` height by about two pixels while retaining the larger seconds.
- Replaced the stacked numeric month with a three-letter uppercase abbreviation.

## 0.1.13 - 2026-07-17

### Device experience

- Added a shared landscape face with a larger clock, pet, and `RUN` / `ASK` / `NEW` task dashboard.
- Added a 2x2-dot Codex allowance meter with separate remaining and consumed colors.
- Added one completion chime per full Codex turn, controlled by the device sound setting.
- Added compact automatic OTA progress and hardened display transitions, clock rendering, and unread-count handling.

### Host and protocol

- Added Codex account rate-limit monitoring for five-hour and seven-day remaining allowance.
- Added read-only Codex Desktop task discovery, unread counts, and de-duplicated Desktop completion signals.
- Added signed Mac-push and automatic Wi-Fi OTA with pinned trust, one-time manifests, boot-health validation, and rollback protection.
- Hardened account-monitor startup, cancellation, retry, BLE ownership, oversized snapshots, and packaged firmware handling.

### Packaging

- Added distinct USB recovery (`*-full.bin`) and OTA (`*-app.bin`) firmware artifacts.
- Embedded and validated the host/firmware version as `0.1.13`.

## 0.1.4 - 2026-04-26

- Improved managed Codex launch reliability on macOS and cleaned up process groups after bridge shutdown or startup failure.
