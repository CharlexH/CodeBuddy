# Codex Usage Meter Design

## Goal

Show the current Codex 5-hour and 7-day remaining allowances as two clearly separated bottom bars on StickS3, without exposing account credentials to the device.

## Data source

`code-buddy` will use the supported Codex app-server account API rather than estimating usage from token counts or calling private ChatGPT HTTP endpoints. The long-lived host agent will own one local, loopback-only `codex app-server` monitor connection, initialize it, and then:

1. Call `account/rateLimits/read` on startup and on a bounded refresh cadence.
2. Merge sparse `account/rateLimits/updated` notifications into the last full response.
3. Select the `codex` bucket from `rateLimitsByLimitId` when present, otherwise use the backward-compatible `rateLimits` value.
4. Convert `primary.usedPercent` and `secondary.usedPercent` to remaining percentages, preserving the returned window duration and reset time for validation and freshness decisions.

The monitor must use the resolved real Codex binary, never Code Buddy's shim. It sends only the two rounded remaining percentages to the BLE snapshot. It never reads `~/.codex/auth.json`, decodes tokens, or exposes an HTTP endpoint.

The monitor keeps the last complete snapshot through a short reconnect. The device meter is omitted until the first valid complete snapshot and after the snapshot exceeds its freshness bound, so a disconnected host cannot show a known-stale allowance. A failing rate-limit fetch must not disrupt approvals, session discovery, or BLE publication.

## Host architecture

Add a small account-usage monitor independent of managed Codex turns. This keeps the meter current for the same signed-in account even while the user uses Codex Desktop or an existing CLI session that Code Buddy only observes.

The monitor owns the extra local app-server process and websocket connection; the existing managed bridge continues to own the approval proxy. A pure `UsageLimits` model parses camelCase app-server payloads, validates finite percentages, merges sparse updates, and exposes a displayable pair only when both the approximately 5-hour primary window and approximately 7-day secondary window are available. The monitor injects that model into `BuddyAgent`, which adds compact optional `usage` fields to `BuddySnapshot` before the normal BLE send and state-persistence paths.

This source of truth is intentionally separate from `thread/tokenUsage/updated` and session JSONL. Those values measure per-session tokens or last-observed activity; neither is an account allowance.

## Device protocol and rendering

BLE snapshots gain an optional compact object:

```json
{"usage":{"five_hour_remaining":72,"seven_day_remaining":91}}
```

The firmware treats the object as atomic: it renders nothing unless both values are integers in `0..100`. Older hosts simply omit it, and older firmware ignores it.

Each bar is six physical pixels high. Both bars are inset two pixels from the left and right edges, separated vertically by two pixels, and the lower bar is inset two pixels from the physical bottom. The complete reserved footprint is therefore sixteen pixels high:

- upper bar: `x=2`, `y=screen-height-16`, `width=screen-width-4`, `height=6`; bright green from the left through the 5-hour remaining percent, with a near-black grey-green unfilled portion;
- two-pixel background gap;
- lower bar: `x=2`, `y=screen-height-8`, `width=screen-width-4`, `height=6`; deep green from the left through the 7-day remaining percent, with the same near-black grey-green unfilled portion;
- two-pixel background margin below the lower bar.

The two bars remain independent because the 5-hour and 7-day percentages are separate rolling windows. The full sixteen-pixel footprint is reserved above approval footer text and cleared as one region when usage becomes unavailable. It is painted last on portrait sprites and on direct landscape LCD paths, including the normal HUD, approval card, clock, menus, and settings, without changing button targets.

## Failure behavior

- No ChatGPT-managed Codex authentication, no usable `codex` bucket, malformed values, incomplete pair, or stale monitor state: do not draw the meter.
- Sparse updates retain fields absent from the notification; explicit valid replacements update only their own window.
- A monitor failure backs off and retries. The UI continues to show the last complete fresh pair, then hides rather than inventing a token-derived value.
- Any rate-limit error is logged locally without leaking tokens or account data to BLE, serial output, CLI status, or the device.

## Testing

Unit tests will cover the pure payload parser/model (complete read, codex-bucket selection, legacy shape, sparse update merge, invalid values, and freshness) and the `BuddySnapshot` BLE compatibility behavior. Firmware host-side tests will cover JSON parsing validity and exact integer fill widths, including 0%, 100%, malformed, and absent usage cases. The existing Python suite and a full StickS3 PlatformIO build will validate integration.

## Research basis

The official `openai/codex` app-server README documents `account/rateLimits/read` and `account/rateLimits/updated`, defines `usedPercent`, window duration, and reset time, and marks updates as sparse. The reviewed `pengchujin/esp8266-ai` project contributed only the general snapshot/merge/reconnect pattern. Its direct private usage endpoint and credential-file parsing are deliberately excluded.
