# Landscape Status Dashboard Design

## Goal

Refine the StickS3 landscape shared clock face into a compact Codex dashboard: keep the pet on the left half of the top region, show local task status counts on the right, move time and date slightly lower, and enlarge the single usage meter to a visually dominant 20 px bar.

## Product boundaries

- The device is display-only. It never marks a task read and adds no acknowledgement gesture.
- `RUN` counts local Codex tasks currently executing.
- `ASK` counts local Codex tasks waiting for approval or user input.
- `NEW` mirrors the local Codex Desktop unread-thread collection. Opening a task in the client is the only action that clears its unread state.
- Remote-host unread threads are excluded so `RUN`, `ASK`, and `NEW` share the same local scope.
- Missing or malformed client state must not create a false count or write anything back to Codex state.

## Language

OpenAI product language describes in-progress work and reviewing completed tasks. The app-server protocol exposes active threads plus `waitingOnApproval` and `waitingOnUserInput` active flags, but it does not expose read state. For the 40 px status columns, use the compact display labels:

- `RUN` for active work.
- `ASK` for work that requires user action.
- `NEW` for local unread tasks owned by the Codex client.

This keeps the display human-readable rather than exposing internal enum names or unclear contractions such as `APV`.

References:

- https://openai.com/index/introducing-upgrades-to-codex/
- https://help.openai.com/en/articles/11096431
- Local `codex-cli 0.128.0 app-server generate-json-schema` output for `ThreadStatus` and `ThreadActiveFlag`.

## Data flow

1. The existing session catalog continues to produce local `running` and `waiting` counts.
2. A read-only Codex Desktop state watcher reads `~/.codex/.codex-global-state.json`.
3. It validates `electron-persisted-atom-state.unread-thread-ids-by-host-v1.local` as an array of unique string thread IDs and reports its length.
4. If the file is temporarily unreadable, malformed during replacement, or the internal key disappears after a client update, the watcher retains its last trusted value for the current process. Before the first trusted read, `unread` is omitted rather than synthesized as zero.
5. `BuddySnapshot` publishes the optional count as `"unread"` in the BLE payload.
6. Firmware validates `unread` as an integer from 0 through 255, stores it as `unreadCount`, and renders it under the `NEW` label. Counts above 99 render as `99+`.

Code Buddy never writes to Codex Desktop state.

## Landscape layout

The surface remains 240 x 135.

### Top region: y 0 through 57

- Pet region: x 0 through 119, width 120, height 58.
- Status region: x 120 through 239, width 120, height 58.
- The status region contains three 40 px columns in `RUN`, `ASK`, `NEW` order.
- Labels use the default fixed font at text size 1, centered and dim gray.
- Counts use text size 2, centered below their labels.
- Counts render as decimal values through 99 and `99+` above 99.

### Status colors

- `ASK`: amber/orange, highest priority because it requires user action.
- `NEW`: cyan, second priority because it represents unseen results.
- `RUN`: bright green, normal healthy activity.
- A zero count uses dim gray regardless of category.
- Labels remain dim gray so color is supportive, not the only carrier of meaning.

### Clock and date

- Move `HH:MM` and seconds down from y 73 to y 77.
- Move the time center from y 89 to y 93.
- Move month from y 70 to y 74.
- Move day from y 90 to y 94.
- Preserve current font sizes and horizontal positions.
- The final glyph boxes end no lower than y 110, leaving at least one pixel before the meter region.

### Usage meter

- Reserve y 111 through 134, total height 24.
- Draw one bar at x 2, y 113, width 236, height 20.
- Keep 2 px top, bottom, left, and right insets.
- Use the existing seven-day remaining color and consumed background semantics.
- The landscape shared clock face is intentionally single-window. Do not change portrait meter geometry or approval-screen footer geometry.

## Rendering and invalidation

- Add the status counts to the retained shared-clock cache so only the status region redraws when a count changes.
- Pet frames clear and redraw only the left 120 x 58 region.
- Status redraw clears only the right 120 x 58 region.
- Clock/date and meter layers keep their current independent invalidation behavior.
- Orientation changes and full repaints redraw every region.

## Failure behavior

- No Codex state file: omit `unread`; firmware shows `NEW 0` in dim gray.
- Malformed state during an atomic client update: retain the last trusted count and retry on the next poll.
- Client schema changes: retain the last trusted value for the process and log a bounded warning; never mutate client state.
- BLE payload without `unread`: old hosts and firmware remain compatible.
- Values outside the firmware range: ignore the malformed field instead of corrupting the whole snapshot.

## Acceptance criteria

- Landscape top region is split into equal 120 px pet and status halves.
- `RUN`, `ASK`, and `NEW` display local counts and use the approved priority colors.
- Opening a local unread task in Codex Desktop reduces `NEW` on the device after the next host poll and BLE publish.
- No device button changes unread state.
- Time and date move down exactly 4 px without touching the usage meter.
- The landscape shared clock shows one 20 px usage bar inside a 24 px footprint.
- Portrait, approval, OTA, menu, settings, and passkey layouts remain unchanged.

