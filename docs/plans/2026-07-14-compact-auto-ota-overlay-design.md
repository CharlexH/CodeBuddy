# Compact Auto OTA Overlay Design

## Goal

Keep automatic OTA progress visible without reusing the portrait-only 124x124 confirmation window or forcing a landscape clock/runtime surface back to portrait.

## Interaction

- `auto ota = on`: show a compact read-only progress overlay as soon as a verified update starts.
- `auto ota = off`: retain the existing full confirmation window with A/B actions.
- The compact overlay contains only the target version, current phase, progress bar, and percentage.
- Existing cancellation, boot-commit, restart, and boot-health rules remain unchanged.

## Layout

- Portrait: 119x52 at x=8, vertically centered.
- Landscape: 160x44 at x=40, vertically centered.
- Both use text size 1 and a six-pixel progress bar.
- The overlay is centered so it avoids the persistent bottom usage meter and remains legible over either pet/clock surface.

## Rendering

Automatic OTA owns a separate `otaCompactOverlay` flag rather than `otaReceiveScreen`. Shared clock/runtime orientation remains eligible while that flag is active. The same compact content renderer draws onto the portrait sprite or the rotated landscape LCD after the underlying surface has rendered.

## Verification

Pure C++ tests cover Direct-vs-Ask selection and exact portrait/landscape geometry. PlatformIO must build the complete firmware, then OTA readback must report firmware 0.1.8 with valid boot health.
