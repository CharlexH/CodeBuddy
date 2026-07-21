# Changelog

## 0.1.30 - 2026-07-21

- Replaced the discrete 20-second activity bars with a continuous anti-aliased heartbeat curve that smoothly departs from and returns to the center baseline.

## 0.1.29 - 2026-07-21

- Replaced fractional, non-uniform scaling of the 1-bit dashboard font with native-size JetBrains Mono bitmaps for the status, time, seconds, date, task labels, and counts.
- Kept the 8 pt Regular ASCII subset for legacy screens while adding tightly scoped 6, 7, 14, and 20 pt dashboard subsets, avoiding distorted strokes for about 3 KiB of additional flash.

## 0.1.28 - 2026-07-21

- Rebuilt the 240-by-135 landscape dashboard from the Figma layout with a four-state `RUNNING` / `WAITING` / `IDLE` / `OFFLINE` indicator, a compact time and full-date composition, and tinted `RUN` / `ASK` / `NEW` cards.
- Added an optional 20-bit `activity20` snapshot field so the device can render the most recent 20 seconds of real managed and Codex Desktop activity independently from the BLE keepalive.
- Added four quarter-minute blocks above the seconds display and preserved the existing animated 29-by-3 quota matrix with updated four-pixel side and bottom spacing.
- Expanded the reproducible JetBrains Mono ASCII asset to include Regular and Medium weights while increasing firmware size by only about 3 KiB.

## 0.1.27 - 2026-07-20

- Added a reproducible JetBrains Mono Regular ASCII subset for the landscape clock, date, and task dashboard, licensed under SIL Open Font License 1.1.
- Consolidated non-ASCII UI text on the single proportional `efontCN_12` face and removed the smaller and larger Chinese faces from the linked firmware image.
- Reduced firmware flash usage by about 408 KiB versus v0.1.25 while preserving the small built-in fallback font for compact utility screens.

## 0.1.25 - 2026-07-20

- Changed the running quota matrix's bottom row from warm yellow to lavender `#CB9DFF`.

## 0.1.24 - 2026-07-20

- Set the running quota matrix rows to RUN green, blue, and warm `#FAD297` from top to bottom.

## 0.1.23 - 2026-07-20

- Tuned the running quota animation's dim floor to 0.68 for a clearer wave while retaining a continuous remaining-quota region.

## 0.1.22 - 2026-07-20

- Raised the running quota animation's dim floor from 0.40 to 0.80 so the remaining region stays visually continuous while the diagonal wave passes through it.

## 0.1.21 - 2026-07-20

- Restored the landscape quota matrix to the original full-width region and enlarged dots to 6 px.
- Used three rows with 2 px gaps, extending the matrix two pixels upward while keeping its bottom edge fixed.

## 0.1.20 - 2026-07-20

- Changed the landscape quota display to the selected 15-by-3 matrix with 4 px cells, 2 px gaps, and centered 88-by-16 px geometry.
- Matched the Mint diagonal/top-down preset with a 750 ms period and 0.40 dim floor, while keeping the bottom row aligned with the bright RUN green.

## 0.1.19 - 2026-07-19

- Replaced the single vertical quota shimmer with tiled diagonal wave blocks that move right across the full remaining-dot matrix.
- Changed the running gradient to progress by row from RUN green to blue-green and matched the reference animation's roughly 800 ms period.

## 0.1.18 - 2026-07-19

- Added a left-to-right green-to-blue-green shimmer across remaining landscape allowance dots while tasks are running; consumed dots remain static and idle dots return to RUN green.
- Pinned the validated direct display and firmware library versions used by the release build.

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
