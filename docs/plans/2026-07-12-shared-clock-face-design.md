# Shared Standby and Runtime Clock Face Design

## Goal

Make the standby clock and normal Codex-running screen use one shared visual component instead of two separate surfaces.

The shared face contains only:

- the current pet animation;
- one-line `HH:MM:SS` time;
- the existing date line;
- the two usage meters at the bottom.

No normal status label, transcript, scroll indicator, or session message returns. Approval, pairing, menus, settings, reset, info, pet management, and OTA progress remain functional override surfaces.

## Shared behavior

Standby and active Codex runtime call the same renderer, use the same geometry, time formatting, colors, meter placement, and explicit repaint cache. They differ only in the pet persona state: standby continues its idle/sleep schedule while active Codex work uses the derived busy/waiting state.

The renderer is parameterized by orientation and render state rather than using `drawClock()` globals or function-local statics. Standby and active callers keep independent cache instances so orientation entry, seconds ticks, pet animation, and meter invalidation cannot contaminate one another.

An approval prompt replaces the shared face with the existing approval card. Prompt exit forces a complete shared-face repaint. Functional screens and OTA progress have the same priority.

## Geometry

### Portrait 135×240

- Compact pet/peek area: `x=0..134`, `y=0..89`.
- Time: total width about 96 px at text size 2, centered across the screen around `y=174`.
- `HH:MM` uses the primary text color; `:SS` immediately follows at the same size using the dim text color.
- Date remains centered beneath the time around `y=202`, text size 1 and dim color.
- Usage meters remain at `y=224` and `y=232` and are painted last.

### Landscape 240×135

- Compact pet area: `x=0..114`, `y=0..89`.
- Time occupies the right pane, about `x=129..225`, centered around `y=54`, text size 2; seconds are the same size and dimmer.
- Date remains centered in the right pane around `y=86`, text size 1 and dim color.
- Usage meters remain at `y=119` and `y=127` and are painted last.

Invalid or not-yet-synchronized time renders `--:--:--` and the guarded placeholder date; it never trusts invalid RTC fields.

## Rendering details

- Reuse `clockFormatHm()`, `clockFormatSeconds()`, date guards, host-synchronized `_clkTm`, compact ASCII rendering, compact GIF rendering, and the existing usage-meter cache.
- Extract a pure layout and repaint-decision helper before wiring hardware drawing.
- Portrait runtime must stop using the full `buddyTickRuntime()` clear because it would erase time/date; compact pet clearing is limited to the top pet box.
- Landscape runtime uses the compact left pet renderer rather than the full runtime viewport.
- Redraw time/date only on seconds change or full repaint; animate pet at the existing 5 fps cadence.
- Paint meters last and leave approval geometry unchanged.

## Verification

Tests lock portrait/landscape bounds, same-size time segments, date retention, meter separation, idle/active renderer selection, approval/functional-screen priority, orientation/cache isolation, seconds-only refresh, prompt-exit repaint, and invalid-time placeholders. Full firmware tests and PlatformIO build must pass before flashing.

