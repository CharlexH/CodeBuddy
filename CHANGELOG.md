# Changelog

## 0.1.41 - 2026-07-23

- Kept the last valid Codex allowance visible when a fresh account-rate-limit read is temporarily unavailable, malformed, or reported as `null`.
- Restored the last valid allowance from the Mac bridge state after an agent restart and stopped emitting destructive usage clears.
- Preserved the device's in-memory quota meter across BLE disconnects so the portrait bar and landscape dot matrix no longer disappear with task telemetry.

## 0.1.40 - 2026-07-22

- Replaced the synthetic landscape heartbeat with a fixed-scale, 64-sample trace of real input-plus-output token consumption over the latest 20 seconds.
- Added a compact versioned BLE token window with per-session baselines, reset-safe delta tracking, logarithmic intensity mapping, strict firmware decoding, and local aging between host snapshots.
- Kept the curve inside the existing 64-by-14 region, moving newest samples in from the right with upward-only green-to-mint peaks and falling back to the legacy activity trace for older hosts.
- Stabilized auto-oriented home surfaces by resolving strong startup poses before the first frame, retaining the last home orientation across portrait-only pages, and suppressing speculative portrait rendering while the IMU is ambiguous.

## 0.1.39 - 2026-07-22

- Rendered portrait `HH:MM:SS` as one centered, contiguous row using the same native 14pt font and baseline for every digit and colon.
- Preserved the existing color hierarchy by keeping seconds dim while hours and minutes remain primary.

## 0.1.38 - 2026-07-22

- Restored the portrait home screen's original 90px-high, 1x ASCII pet geometry after the v0.1.37 sizing experiment.
- Fixed the actual horizontal distortion by resetting the pet renderer to its built-in 6x8 bitmap font before applying six-pixel character-width centering.
- Retained the enlarged native-size portrait clock and date typography introduced in v0.1.37.

## 0.1.37 - 2026-07-22

- Temporarily enlarged the portrait ASCII pet while diagnosing its alignment; v0.1.38 restores the original geometry and fixes the underlying font mismatch instead.
- Enlarged portrait `HH:MM` to the native 14pt numeric font and `:SS` plus the date line to native 8pt, removing the previous 0.75x and 0.38x fractional downscaling.
- Kept the portrait quota footer at its existing position and retained the landscape heartbeat orientation fix from v0.1.36.

## 0.1.36 - 2026-07-22

- Fixed the landscape heartbeat so every new activity pulse keeps the same upward orientation instead of alternating above and below the centerline according to the wall-clock second.
- Preserved the continuous right-to-left strip-chart motion, off-screen rendering, and the restored JetBrains Mono typography and spacing.

## 0.1.35 - 2026-07-22

- Restored the landscape dashboard's JetBrains Mono Regular/Bold labels after the built-in pixel fonts proved too coarse on the physical display.
- Retained the slashed-zero numeric glyphs, two-pixel status-dot radius, and four-pixel dot-to-label gap from the preceding visual pass.

## 0.1.34 - 2026-07-22

- Changed the Codex heartbeat to a true strip chart: stable historical samples move left while the newest sample enters from the right, without reshaping old peaks.
- Rendered the complete 64-by-14 heartbeat into an off-screen sprite before presenting it, removing the visible clear-and-redraw flash.
- Increased the OTA I/O idle tolerance from 5 to 15 seconds so short Wi-Fi stalls do not abandon an otherwise healthy firmware download.
- Switched dashboard English labels to crisp built-in pixel fonts while retaining JetBrains Mono for numbers and time punctuation, with its OpenType slashed-zero alternate baked into every generated size.
- Reduced the 8-by-8 status dot corner radius to two pixels and tightened the dot-to-label gap to four pixels without moving the dot center.

## 0.1.33 - 2026-07-21

- Moved the landscape status label down one pixel and reduced its indicator from 12-by-12 to 8-by-8 while preserving the original vertical center.
- Increased the continuous Codex heartbeat amplitude from five to six pixels within the existing 64-by-14 region.

## 0.1.32 - 2026-07-21

- Moved the seconds and quarter-minute indicators up two pixels and raised the status label by two pixels without moving its status dot.
- Made the 20-second Codex heartbeat a full-width continuous curve, refreshed independently at up to 20 FPS with subsecond interpolation for faster, smoother activity feedback.

## 0.1.31 - 2026-07-21

- Moved the seconds value down four pixels and changed it to 60% white, while making the date and weekday fully white.
- Switched `RUN` / `ASK` / `NEW` labels to a native JetBrains Mono Bold subset and rendered zero counts in 40% white instead of their task color.

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
