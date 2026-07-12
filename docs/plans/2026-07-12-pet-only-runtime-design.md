# Pet-Only Runtime Design

## Goal

Make the normal Codex runtime screen a quiet pet-first surface: no status/transcript HUD in portrait or landscape, only the existing animated pet plus the two usage bars. Functional screens such as approval, pairing, clock, menus, settings, info, and reset keep their required text.

## Scope

The removed text is the normal runtime HUD: `Codex is working`, `No active Codex turn`, recent output lines, message summaries, and their scroll indicator. Approval command text and A/B actions are not status HUD and remain unchanged. The INFO pages may continue to expose diagnostic details intentionally selected by the user.

The visible `transcript` setting is removed because the runtime HUD is no longer optional. The persisted `Settings.hud` field and NVS key remain readable/writable for backward compatibility with already provisioned devices, but runtime rendering no longer consults it.

## Pet layout

The enlarged usage bars reserve the bottom sixteen pixels. The normal pet viewport is therefore:

- portrait: `135 x 224`, centered at `(67, 112)`;
- landscape: `240 x 119`, centered at `(120, 59)`.

Use the largest crisp native/integer scale that fits rather than stretching pixels fractionally:

- ASCII pet, portrait: retain the existing maximum safe `2x` glyph scale and vertically center its 164-pixel animation canvas with a 30-pixel offset;
- ASCII pet, landscape: use `1x`, center it horizontally at `x=120`, and vertically center its 82-pixel animation canvas with an 18-pixel offset;
- GIF pet, portrait: render at native `1x` and center inside the 224-pixel viewport;
- GIF pet, landscape: use native `1x` only when the GIF fits within `236 x 119`; otherwise use the existing half-scale renderer, centered at `(120, 59)`.

This prioritizes sharpness and avoids clipping animation particles into the usage bars. Approval mode retains the existing compact/upper placement so the pet cannot obscure the command and action labels.

## Rendering and transitions

Portrait normal mode does not call `drawHUD()`. Landscape runtime scheduling treats only an approval prompt as an overlay; transcript/message changes no longer trigger direct-LCD repaint work.

Removing the HUD creates more persistent background, so transitions must explicitly clear it:

- leaving an approval prompt performs one full normal-surface clear/invalidation before the pet resumes;
- leaving a menu/settings/info overlay uses the existing invalidation path, extended where necessary so no stale panel or old HUD pixels remain;
- landscape prompt exit performs a full runtime repaint rather than clearing only the former text rectangle;
- usage bars remain the final paint operation after the pet or approval surface.

## Compatibility

The host/BLE payload, approval decisions, pet species/state selection, animation cadence, usage percentages/colors, orientation debounce, clock behavior, and device settings storage format remain unchanged. No device flash or host installation occurs until the user finishes specifying the remaining changes.

## Testing

Pure firmware tests will cover:

- normal portrait/landscape surfaces never request a status overlay;
- approval surfaces still request an overlay;
- exact portrait and landscape pet viewport/scale/offset decisions;
- prompt exit requests a full repaint;
- the settings menu no longer exposes a transcript row while later actions retain their intended indices.

All existing firmware host tests and a full PlatformIO build must pass before the local-only commit is considered ready.
