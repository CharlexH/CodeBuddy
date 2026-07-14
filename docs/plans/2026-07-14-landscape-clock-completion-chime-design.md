# Landscape Clock and Completion Chime Design

## Goal

Make the StickS3 landscape clock materially easier to read while preserving the bottom usage meter, and play one compact sound after each successfully completed Codex turn.

## Scope

- Change only the landscape shared clock face. Portrait geometry stays unchanged.
- Treat a task as one complete Codex turn, not an individual tool, command, or item.
- Play the completion sound for successful terminal turns. Interrupted or failed turns do not use the success chime.
- Preserve functional surfaces such as approval, menu, settings, passkey, and OTA progress.

## Landscape Geometry

The landscape display is `240 x 135`. The bottom `16 px` remains reserved for the existing usage meter, leaving a `240 x 119` clock surface.

- Pet region: `x=50`, `y=0`, `width=140`, `height=58`.
  - The pet is horizontally centered at `x=120`.
  - ASCII pets stay at integer scale `1`.
  - Compact GIF/text rendering uses the existing clipped pet renderer.
- Time/date row: `y=60..118`.
  - `HH:MM` starts at `x=8` and uses default-font text size `4`, yielding an approximately `120 x 32 px` time block.
  - Landscape seconds are omitted to prioritize the primary time.
  - Month and day are rendered as separate two-digit values on the right: month above day, both at text size `2`.
- Usage meter: unchanged at `y=119..134` with the existing `16 px` footprint.

All rectangles are non-overlapping so pet animation refreshes cannot erase time, date, or usage pixels.

## Completion Event Transport

The host sends a monotonic `completion_seq` value in normal buddy snapshots.

- Managed Codex sessions increment the sequence for a unique successful `turn/completed` event.
- `item/completed` is ignored because it represents individual commands or tool items inside a turn.
- Repeated heartbeats, catalog visibility, and reconnects retain the same sequence rather than creating another completion.
- Firmware treats the first received sequence as a baseline and does not chime on initial connection. A later sequence change plays the sound once.

The sequence avoids the duplicate and missed-edge behavior of a persistent boolean `completed` field.

## Completion Sound

Use the existing M5Unified speaker with no audio asset:

- `1600 Hz` for roughly `70 ms`
- followed by `2400 Hz` for roughly `100 ms`
- queue the tones without blocking the render/BLE loop
- respect the existing persisted `sound` setting

No WAV, MIDI parser, or new media asset is added.

## Validation

- Pure C++ layout tests pin the new landscape rectangles, font sizes, and meter boundary.
- Firmware completion-sequence tests cover initial baseline, one chime per change, duplicate snapshots, and wrap-safe changes.
- Host tests cover terminal turn status parsing, sequence increments, payload serialization, and no increment for item completion/interruption.
- Run focused tests, the full host suite, all firmware unit tests, and `pio run`.
- Device flashing is a separate checkpoint; a local build alone is not physical-screen or speaker proof.
