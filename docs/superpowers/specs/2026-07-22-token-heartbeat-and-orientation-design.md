# Token Heartbeat and Orientation Stability Design

## Scope

This change has two independently testable parts that will ship together:

1. Replace the landscape dashboard's binary activity heartbeat with a smooth visualization of recent input-plus-output token consumption.
2. Prevent auto-oriented surfaces from drawing an incorrect portrait frame before switching to landscape at startup or after leaving a portrait-only surface.

The landscape dashboard geometry, portrait-only menus, existing color hierarchy, and quota footer remain unchanged.

## Token heartbeat

### Meaning

The graph represents newly consumed input and output tokens over the most recent 20 seconds. It does not represent account allowance or quota percentage.

Each task is tracked independently by session ID. The host records only positive changes in a session's cumulative total. A newly discovered session establishes a baseline without emitting a historical spike. A lower counter, process restart, compaction reset, or disappearing session also causes a rebaseline rather than a negative or artificial positive sample.

Managed sessions should use total token usage when the Codex event supplies it. If only separate input and output counters are available, the host sums them. Read-only Desktop sessions use the session log's total token count. Missing token data produces no token sample.

### Sampling and transformation

- Window: 20 seconds.
- Samples: 64 rolling bins, approximately 312.5 milliseconds per bin.
- Raw value: the sum of positive token deltas received in each bin.
- Smoothing: distribute each delta across three adjacent bins with weights 20%, 60%, and 20%. The weights sum to one, preserving the represented total while adding approximately one bin of visual latency.
- Scale: a fixed continuous logarithmic transform, not adaptive normalization. Approximately 100 tokens form a low peak, 1,000 tokens a medium peak, 10,000 tokens a high peak, and 32,000 tokens approach full amplitude.
- Encoded intensity: 0 through 255 per sample after the fixed transform.

Fixed scaling keeps different tasks and different moments comparable. Values above the reference ceiling are clipped visually but remain harmless to the cumulative token counters.

### Host and BLE data flow

A dedicated host component owns per-session baselines and the rolling token window. The agent updates it whenever managed or read-only session token totals change. The current 64 intensities are serialized into one compact versioned BLE field.

The encoding must keep a normal snapshot well below the firmware's 2,048-byte receive ring and work with the existing 180-byte chunking transport. The decoder must reject malformed lengths or characters atomically, leaving the previous valid curve intact.

Backward compatibility is bidirectional:

- New firmware with an old host continues to use the existing `activity20` heartbeat.
- An old firmware ignores the new field.
- New firmware switches to token mode only after receiving a valid token window.

### Firmware rendering

The graph keeps its existing 64-by-14-pixel region and centerline. Samples move from right to left, with the newest value entering at the right edge. Peaks always move upward; token intensity never changes sign.

The firmware interpolates the 64 values into a continuous anti-aliased line. Height is the primary encoding. Green brightness supplies additional perceived levels within the limited six-to-seven-pixel vertical amplitude, and the strongest peaks may move slightly toward mint. A zero sample returns to the centerline.

The device advances the window locally between BLE snapshots, so stale samples leave the left side at a steady rate instead of freezing. Rendering remains off-screen and is presented atomically to avoid flicker.

## Orientation stability

### Root cause

The current auto-orientation state starts at portrait and is reset to portrait whenever an auto-oriented surface becomes temporarily ineligible. A sideways device then needs about 15 stable IMU frames before landscape is selected. During that interval the renderer is allowed to draw the portrait home screen, causing the visible portrait-to-landscape flash.

### Initial orientation resolution

Auto-oriented surfaces gain an explicit unresolved state. Before the first frame of a newly eligible surface:

- A clearly sideways gravity vector selects landscape rotation 1 or 3 immediately.
- A clearly upright vector selects portrait immediately.
- An ambiguous vector retains the most recent stable auto orientation.
- On cold boot with no stable history and an ambiguous vector, the existing background remains visible until orientation resolves; no incorrect layout is drawn.

After the initial orientation is resolved, the existing hysteresis remains responsible for ordinary portrait/landscape transitions and left/right landscape swaps.

### State ownership

Portrait-only menus, settings, Wi-Fi, reset, and full OTA screens may temporarily set the display to portrait, but they must not erase the last stable auto-home orientation. Returning to an auto-oriented surface performs a fresh strong-pose classification before its first draw.

Standby clock, active/waiting shared face, and landscape approval use the same initial resolver. Forced portrait and forced landscape settings bypass the unresolved state and select their configured orientation immediately.

### Transition audit

The implementation will cover these paths with one shared policy rather than page-specific patches:

- cold startup while already sideways;
- standby home entry;
- active or waiting task home entry;
- approval entry and exit;
- return from menu, settings, Wi-Fi, reset, OTA receive, and OTA progress surfaces;
- automatic portrait-to-landscape and landscape-to-portrait changes;
- landscape rotation 1 to rotation 3 changes;
- forced portrait and forced landscape settings;
- ambiguous IMU readings and sensor noise.

## Error handling

- Invalid or stale token payloads never clear a previously valid graph.
- Session counter decreases rebaseline without drawing.
- Token values use saturating arithmetic to avoid overflow.
- BLE disconnects let the device age the existing curve to zero.
- Ambiguous orientation never causes a speculative layout draw.
- Orientation state resets only on explicit policy changes or hardware reinitialization, not ordinary page transitions.

## Testing

Host tests will cover per-session deltas, first-observation baselines, resets, concurrent sessions, 64-bin aging, smoothing conservation, fixed logarithmic mapping, encoding, and malformed data.

Firmware-native tests will cover decoding, aging, intensity-to-height/color mapping, continuous right-to-left movement, upward-only peaks, old-host fallback, and bounded drawing inside the existing heartbeat rectangle.

Orientation tests will cover every transition listed above, especially the invariant that a clearly sideways first sample cannot produce a portrait frame. Existing hysteresis and forced-orientation tests remain in place.

Release verification requires the complete host and firmware-native suites, a PlatformIO release build, artifact version inspection, OTA completion, and a fresh device readback reporting the new version as running and healthy. Physical review should confirm that the token curve is smooth and that startup/page returns no longer flash through portrait.

## Non-goals

- Changing the 64-by-14 heartbeat region or the rest of the landscape layout.
- Turning token consumption into account quota percentage.
- Adaptive per-window normalization.
- Counting historical tokens when a session is first discovered.
- Making portrait-only menus rotate.
