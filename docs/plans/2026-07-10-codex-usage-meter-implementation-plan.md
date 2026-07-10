# Codex Usage Meter Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Surface the signed-in Codex account's current 5-hour and 7-day remaining limits as one flush, dual-lane StickS3 bottom meter.

**Architecture:** A long-lived, loopback-only account monitor runs the real Codex app-server and consumes the documented `account/rateLimits/read` request plus sparse update notification. A pure model validates, merges, and converts account-limit data into optional BLE percentages. Firmware accepts the optional pair atomically and paints two touching lanes at the bottom edge after every screen surface.

**Tech Stack:** Python 3.9+, asyncio, websockets 15, Codex app-server JSON-RPC, ArduinoJson 7, M5Unified/PlatformIO.

---

### Task 1: Specify and test account-limit normalization

**Files:**
- Create: `src/codex_buddy/usage_limits.py`
- Create: `tests/test_usage_limits.py`

**Step 1: Write the failing tests**

Add tests that call the wished-for `UsageLimits.from_read_result(...)` and `merge_update(...)` APIs. Cover a `rateLimitsByLimitId["codex"]` response, the legacy `rateLimits` shape, sparse primary-only update retention, invalid/out-of-range values, an unsupported window duration, and expiry of an otherwise valid pair.

```python
def test_complete_codex_snapshot_exports_remaining_percentages():
    limits = UsageLimits.from_read_result(
        {"rateLimitsByLimitId": {"codex": {"primary": {"usedPercent": 28, "windowDurationMins": 300, "resetsAt": 10}, "secondary": {"usedPercent": 9, "windowDurationMins": 10080, "resetsAt": 20}}}},
        observed_at=100.0,
    )
    assert limits.display_pair(now=101.0) == UsageDisplay(five_hour_remaining=72, seven_day_remaining=91)
```

**Step 2: Run the test to verify it fails**

Run: `.venv/bin/pytest tests/test_usage_limits.py -q`

Expected: FAIL during collection because `codex_buddy.usage_limits` does not exist.

**Step 3: Implement the minimal model**

Create immutable `UsageWindow`, `UsageDisplay`, and `UsageLimits` dataclasses. Parse only `rateLimitsByLimitId["codex"]` or the legacy value; reject non-numeric/non-finite percentages and only display primary/secondary when their returned durations are within a narrow 5-hour/7-day tolerance. Clamp valid values to `0..100`, round `100 - usedPercent`, merge absent update fields without discarding the prior valid window, and suppress a pair past a named freshness interval.

**Step 4: Run the test to verify it passes**

Run: `.venv/bin/pytest tests/test_usage_limits.py -q`

Expected: PASS with every parser, merge, and freshness case green.

**Step 5: Commit**

```bash
git add src/codex_buddy/usage_limits.py tests/test_usage_limits.py
git commit -m "feat: model Codex account usage limits"
```

### Task 2: Add a credential-safe Codex account monitor

**Files:**
- Create: `src/codex_buddy/account_usage_monitor.py`
- Create: `tests/test_account_usage_monitor.py`
- Modify: `src/codex_buddy/bridge.py:32-93`
- Modify: `tests/test_bridge.py:1-91`

**Step 1: Write the failing tests**

Use an injected fake process, readiness probe, and websocket to verify that the monitor starts the resolved real Codex binary with `app-server --listen`, sends `initialize`, `initialized`, then `account/rateLimits/read`, publishes a complete snapshot, and republishes after a sparse `account/rateLimits/updated`. Assert it never receives an auth-file path or token argument. Add bridge-helper coverage before moving the reusable loopback app-server launch/terminate helpers out of `bridge.py`.

```python
async def test_monitor_reads_then_merges_the_official_rate_limit_notification():
    monitor = AccountUsageMonitor("/usr/local/bin/codex", on_usage=seen.append, websocket_connect=fake_connect)
    await monitor.start()
    assert fake_socket.sent[-1]["method"] == "account/rateLimits/read"
    await fake_socket.deliver({"method": "account/rateLimits/updated", "params": {"rateLimits": {"primary": {"usedPercent": 40, "windowDurationMins": 300}}}})
    assert seen[-1].five_hour_remaining == 60
```

**Step 2: Run the tests to verify they fail**

Run: `.venv/bin/pytest tests/test_account_usage_monitor.py tests/test_bridge.py -q`

Expected: FAIL because the monitor and reusable app-server lifecycle API do not exist.

**Step 3: Implement the minimal monitor**

Extract the existing loopback port allocation, Codex process environment, readiness wait, and process-group teardown into a small shared helper used by both bridge classes and the monitor. `AccountUsageMonitor` must:

- launch only the real resolved binary, never the shim;
- open a localhost websocket, complete the app-server initialization handshake, and request the initial snapshot;
- keep one receive loop, merge `account/rateLimits/updated`, and re-read at a bounded interval;
- keep a complete snapshot across transient reconnects, with capped retry backoff;
- terminate its process and cancel its task during `stop()`;
- report only `UsageDisplay` values to its callback and log no auth or account payload.

**Step 4: Run the tests to verify they pass**

Run: `.venv/bin/pytest tests/test_account_usage_monitor.py tests/test_bridge.py -q`

Expected: PASS, including process command, protocol order, sparse-update, and cleanup assertions.

**Step 5: Commit**

```bash
git add src/codex_buddy/account_usage_monitor.py src/codex_buddy/bridge.py tests/test_account_usage_monitor.py tests/test_bridge.py
git commit -m "feat: monitor Codex account rate limits"
```

### Task 3: Thread the optional pair through the agent and BLE snapshot

**Files:**
- Modify: `src/codex_buddy/reducer.py:26-91`
- Modify: `src/codex_buddy/agent.py:162-238,389-432`
- Modify: `tests/test_reducer.py`
- Modify: `tests/test_agent.py`

**Step 1: Write the failing tests**

Add an optional `usage` argument to the expected snapshot. Verify an absent pair preserves the previous exact BLE shape and a valid pair emits only the two remaining percentages. Inject a fake account monitor into `BuddyAgent`; verify it starts with the agent, causes a fresh BLE publish when it supplies a pair, and stops during shutdown without changing approval routing.

```python
def test_snapshot_adds_usage_only_when_both_remaining_values_are_known():
    snapshot = BuddySnapshot(..., usage=UsageDisplay(72, 91))
    assert snapshot.as_ble_payload()["usage"] == {"five_hour_remaining": 72, "seven_day_remaining": 91}
```

**Step 2: Run the tests to verify they fail**

Run: `.venv/bin/pytest tests/test_reducer.py tests/test_agent.py -q`

Expected: FAIL because `BuddySnapshot` has no account-usage field and `BuddyAgent` has no monitor lifecycle.

**Step 3: Implement the minimal integration**

Extend `BuddySnapshot` with an optional `UsageDisplay`; preserve its 900-byte compaction priority. Add an injectable monitor factory to `BuddyAgent`, initialize it only after setup has a real Codex path, store the latest displayable value in the agent, and use it whenever publishing, status reporting, and state persistence derive a snapshot. Do not make `ManagedSessionRuntime`, `SessionCatalog`, session-log parsing, approval commands, or device-decision routing responsible for account quota.

**Step 4: Run the tests to verify they pass**

Run: `.venv/bin/pytest tests/test_reducer.py tests/test_agent.py -q`

Expected: PASS with old payload compatibility, new payload, monitor lifecycle, and approval regression tests green.

**Step 5: Commit**

```bash
git add src/codex_buddy/reducer.py src/codex_buddy/agent.py tests/test_reducer.py tests/test_agent.py
git commit -m "feat: publish Codex usage to Buddy snapshots"
```

### Task 4: Parse and validate the optional usage object in firmware

**Files:**
- Create: `firmware/src/usage_meter_logic.h`
- Create: `firmware/tests/usage_meter_logic_test.cpp`
- Modify: `firmware/src/data.h:8-151`

**Step 1: Write the failing firmware host-side test**

Define desired pure helpers for percentage acceptance and pixel fill conversion. Test 0%, 100%, a fractional pixel width, out-of-range values, and atomic absence when either lane is invalid.

```cpp
expect_true(usageMeterFillWidth(135, 72) == 97, "72 percent should use deterministic integer width");
expect_true(!usageMeterValidPair(72, 101), "out-of-range weekly value must suppress the whole meter");
```

**Step 2: Run the test to verify it fails**

Run: `cd firmware && g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage-meter-test && /tmp/usage-meter-test`

Expected: FAIL because `usage_meter_logic.h` does not exist.

**Step 3: Implement the minimal firmware parser**

Create the pure helper header and add `hasUsageLimits`, `fiveHourRemaining`, and `sevenDayRemaining` to `TamaState`. In `_applyJson`, read the optional `usage` object atomically; set the flag only for an integer `0..100` pair and clear it for a malformed present object. Keep old snapshots valid and do not add tokens, timestamps, reset values, account identifiers, or text to the device state.

**Step 4: Run the test to verify it passes**

Run: `cd firmware && g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage-meter-test && /tmp/usage-meter-test`

Expected: exit code 0.

**Step 5: Commit**

```bash
git add firmware/src/usage_meter_logic.h firmware/src/data.h firmware/tests/usage_meter_logic_test.cpp
git commit -m "feat: accept usage meter BLE data"
```

### Task 5: Render the combined flush meter in portrait and landscape

**Files:**
- Modify: `firmware/src/main.cpp:445-943,1506-1525`
- Modify: `firmware/tests/usage_meter_logic_test.cpp`

**Step 1: Write the failing rendering-geometry test**

Extend the pure firmware test with the six-pixel composition contract: top lane is bright 5-hour fill over a near-black grey-green base, bottom lane is deep-green 7-day fill over the same base, both use origin `0` and full display width, and no valid pair yields no draw operations.

**Step 2: Run the test to verify it fails**

Run: `cd firmware && g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage-meter-test && /tmp/usage-meter-test`

Expected: FAIL because the renderer geometry constants/operations are missing.

**Step 3: Implement the minimal rendering helpers**

Add a small generic `drawUsageMeter(surface, width, height)` helper in `main.cpp`. Use a 6-pixel high rectangle at `x=0`, `y=height-6`: three top pixels draw 5-hour bright green/consumed grey-green; three bottom pixels draw 7-day deep green/consumed grey-green. Paint it last before each portrait `pushSprite`, in `drawLandscapeRuntime` before resetting rotation, and in the direct landscape clock path. Do not alter buttons, HUD scroll positions, approval semantics, or any user-owned current changes.

**Step 4: Run the tests and build to verify they pass**

Run:

```bash
cd firmware && g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage-meter-test && /tmp/usage-meter-test
pio run
```

Expected: geometry test exits 0 and PlatformIO returns exit code 0 for `m5stack-sticks3`.

**Step 5: Commit**

```bash
git add firmware/src/main.cpp firmware/tests/usage_meter_logic_test.cpp
git commit -m "feat: draw combined Codex usage meter"
```

### Task 6: Full verification and documentation

**Files:**
- Modify: `README.md` (only if existing user-facing setup/runtime documentation needs a one-sentence usage-meter note)
- Modify: `docs/plans/2026-07-10-codex-usage-meter-design.md` (only if implementation revealed a material protocol difference)

**Step 1: Run the complete host suite**

Run: `.venv/bin/pytest -q`

Expected: exit code 0 with all existing and new tests passing.

**Step 2: Run full firmware verification**

Run: `cd firmware && pio run`

Expected: exit code 0 for the StickS3 build.

**Step 3: Inspect the final diff**

Run:

```bash
git diff --check HEAD~5..HEAD
git status --short
```

Expected: no whitespace errors; only the intended usage-meter files beyond pre-existing user changes.

**Step 4: Commit any necessary documentation correction**

```bash
git add README.md docs/plans/2026-07-10-codex-usage-meter-design.md
git commit -m "docs: document Codex usage meter"
```

Skip this commit if no documentation file changed.
