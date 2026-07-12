# Usage Bar Spacing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Resize the StickS3 Codex usage display into two visible six-pixel bars with exact two-pixel outer and inter-bar spacing.

**Architecture:** Keep the existing host payload and firmware render-state cache unchanged. Update only the pure usage-meter geometry contract so every portrait and direct-LCD landscape surface receives the new rectangles and reserves/clears the complete sixteen-pixel footprint.

**Tech Stack:** C++17 pure geometry tests, Arduino/M5Unified drawing adapters, PlatformIO ESP32-S3 build.

---

### Task 1: Resize and inset both usage bars

**Files:**
- Modify: `firmware/tests/usage_meter_logic_test.cpp`
- Modify: `firmware/src/usage_meter_logic.h`
- Modify: `firmware/src/main.cpp`

**Step 1: Write the failing geometry tests**

Change the portrait `135x240` expectations to:

```cpp
// Five-hour base/fill: x=2, y=224, width=131, height=6.
// Seven-day base/fill: x=2, y=232, width=131, height=6.
// Bottom rows 238..239 and side columns remain background.
```

Assert the 72% fill is `94` pixels (`131 * 72 / 100`) and the 91% fill is `119` pixels. Assert `usageMeterFooterInset(...)` returns `16` when visible. Add a landscape `240x135` assertion with `x=2`, usable width `236`, upper `y=119`, and lower `y=127`.

**Step 2: Run the focused test to verify it fails**

Run:

```bash
cd firmware
g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage_meter_logic_test
/tmp/usage_meter_logic_test
```

Expected: FAIL against the old two touching three-pixel lanes.

**Step 3: Implement the minimal geometry**

Define constants for a 6-pixel lane, 2-pixel gap, 2-pixel side inset, 2-pixel bottom inset, and computed 16-pixel footprint. Generate both base/fill rectangles using `x=2` and `fullWidth-4`; place the upper bar at `fullHeight-16` and lower bar at `fullHeight-8`. Treat widths below five pixels or heights below sixteen pixels as non-renderable.

Update the clear helper in `firmware/src/main.cpp` to erase the complete sixteen-pixel footprint. Existing render caching, colors, percentages, portrait/landscape surfaces, and stale-state transitions stay unchanged.

**Step 4: Run focused and full firmware verification**

Run:

```bash
cd firmware
g++ -std=c++17 -Isrc tests/usage_meter_logic_test.cpp -o /tmp/usage_meter_logic_test
/tmp/usage_meter_logic_test
for test_file in tests/*_test.cpp; do
  test_name="$(basename "${test_file%.cpp}")"
  if rg -q 'ArduinoJson|usage_meter_json' "$test_file"; then
    g++ -std=c++17 -Isrc -I.pio/libdeps/m5stack-sticks3/ArduinoJson/src "$test_file" -o "/tmp/$test_name"
  else
    g++ -std=c++17 -Isrc "$test_file" -o "/tmp/$test_name"
  fi
  "/tmp/$test_name"
done
pio run -s
```

Expected: every test and the StickS3 build exit 0.

**Step 5: Commit locally without deploying**

```bash
git add firmware/src/usage_meter_logic.h firmware/src/main.cpp firmware/tests/usage_meter_logic_test.cpp
git commit -m "fix: enlarge separated usage bars"
```

Do not flash the device, reinstall the host package, or push the branch. Wait for the user's remaining requested changes.
