# Codex Usage Meter Design

## Goal

Show the current Codex 5-hour and 7-day remaining allowances as one flush, bottom-edge meter on StickS3, without exposing account credentials to the device.

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

The meter is six physical pixels high, begins at `x=0`, ends at the rightmost pixel, and is drawn at `y=screen-height-6`. It is visually one bar made of two touching three-pixel lanes with no border or gap:

- upper lane: bright green from the left through the 5-hour remaining percent; its unfilled portion is near-black grey-green;
- lower lane: deep green from the left through the 7-day remaining percent; its unfilled portion is the same near-black grey-green.

The lanes share the same base and edges, so they are a single combined meter rather than two separated controls. It is painted last on portrait sprites and on direct landscape LCD paths, including the normal HUD, approval card, clock, menus, and settings, so it remains flush at the physical bottom without changing the existing layout or touch/button targets.

## Failure behavior

- No ChatGPT-managed Codex authentication, no usable `codex` bucket, malformed values, incomplete pair, or stale monitor state: do not draw the meter.
- Sparse updates retain fields absent from the notification; explicit valid replacements update only their own window.
- A monitor failure backs off and retries. The UI continues to show the last complete fresh pair, then hides rather than inventing a token-derived value.
- Any rate-limit error is logged locally without leaking tokens or account data to BLE, serial output, CLI status, or the device.

## Testing

Unit tests will cover the pure payload parser/model (complete read, codex-bucket selection, legacy shape, sparse update merge, invalid values, and freshness) and the `BuddySnapshot` BLE compatibility behavior. Firmware host-side tests will cover JSON parsing validity and exact integer fill widths, including 0%, 100%, malformed, and absent usage cases. The existing Python suite and a full StickS3 PlatformIO build will validate integration.

## Research basis

The official `openai/codex` app-server README documents `account/rateLimits/read` and `account/rateLimits/updated`, defines `usedPercent`, window duration, and reset time, and marks updates as sparse. The reviewed `pengchujin/esp8266-ai` project contributed only the general snapshot/merge/reconnect pattern. Its direct private usage endpoint and credential-file parsing are deliberately excluded.
