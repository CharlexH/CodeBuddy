# Pet-Only Runtime Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove normal Codex status text globally and center the largest crisp existing pet animation above the separated usage bars.

**Architecture:** A pure runtime-layout helper defines when text overlays are allowed and returns portrait/landscape pet viewport geometry. The existing ASCII/GIF renderers accept explicit runtime placement, while approval and non-runtime screens retain their current layouts. Prompt exits invalidate the entire runtime surface so removed HUD/approval pixels cannot survive.

**Tech Stack:** C++17 pure tests, M5Unified/LovyanGFX, AnimatedGIF, PlatformIO ESP32-S3.

---

### Task 1: Define pet-only runtime layout policy

**Files:**
- Create: `firmware/src/runtime_pet_layout_logic.h`
- Create: `firmware/tests/runtime_pet_layout_logic_test.cpp`

**Step 1: Write the failing policy test**

Define a wished-for API that returns:

```cpp
struct RuntimePetLayout {
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  int16_t centerX;
  int16_t centerY;
  uint8_t asciiScale;
  int16_t asciiYOffset;
};

RuntimePetLayout runtimePetLayout(bool landscape);
bool runtimeStatusOverlayVisible(bool inPrompt);
bool runtimeNeedsFullRepaintOnPromptExit(bool wasPrompt, bool inPrompt);
```

Assert portrait is `135x224`, center `(67,112)`, ASCII scale `2`, offset `30`; landscape is `240x119`, center `(120,59)`, ASCII scale `1`, offset `18`. Assert normal runtime never exposes a status overlay, approval does, and only `true -> false` prompt transition requires the full repaint.

**Step 2: Run the test to verify it fails**

Run:

```bash
cd firmware
g++ -std=c++17 -Isrc tests/runtime_pet_layout_logic_test.cpp -o /tmp/runtime_pet_layout_logic_test
/tmp/runtime_pet_layout_logic_test
```

Expected: FAIL because `runtime_pet_layout_logic.h` does not exist.

**Step 3: Implement the minimal pure helper**

Use fixed StickS3 geometry and the existing 16-pixel meter footprint. Keep the helper independent of Arduino/M5 headers.

**Step 4: Run the test to verify it passes**

Run the same command; expected exit code `0`.

**Step 5: Commit locally**

```bash
git add firmware/src/runtime_pet_layout_logic.h firmware/tests/runtime_pet_layout_logic_test.cpp
git commit -m "feat: define pet-only runtime layout"
```

### Task 2: Apply centered pet rendering and remove runtime HUD

**Files:**
- Modify: `firmware/src/buddy.h`
- Modify: `firmware/src/buddy.cpp`
- Modify: `firmware/src/character.h`
- Modify: `firmware/src/character.cpp`
- Modify: `firmware/src/main.cpp`
- Modify: `firmware/tests/screen_orient_logic_test.cpp`
- Test: `firmware/tests/runtime_pet_layout_logic_test.cpp`

**Step 1: Write failing integration-facing assertions**

Extend the pure test/policy as needed to lock these behaviors before production edits:

- normal landscape has no overlay revision work;
- approval still has an overlay;
- prompt exit forces a complete repaint;
- ASCII placement uses the requested center/offset without changing animation state;
- the normal settings action order is brightness, sound, Bluetooth, Wi-Fi, LED, clock rotation, pet, reset, back (no transcript row).

Run the focused tests and confirm at least one fails against the current HUD/settings/rendering behavior.

**Step 2: Add explicit ASCII placement**

Extend the buddy renderer with an explicit target center, y-offset, and integer scale for direct runtime rendering. Keep existing species coordinates relative to `BUDDY_X_CENTER`; translate them centrally so particles and body move together. Portrait home rendering uses scale `2`, center `67`, offset `30`; landscape non-prompt rendering uses scale `1`, center `120`, offset `18`. Existing clock/INFO/PET/approval call sites keep their current layout.

**Step 3: Add adaptive GIF placement**

Center the native GIF in the portrait `135x224` runtime viewport. Extend direct rendering to choose native `1x` when the GIF fits in `236x119`, otherwise half scale, centered at `(120,59)`. Keep approval and clock placements unchanged.

**Step 4: Remove status HUD and its visible setting**

Stop calling and remove the normal portrait/landscape HUD renderers and transcript-scroll revision work. Landscape overlay scheduling uses only `inPrompt`. Remove the `transcript` row from `settingsItems` and remap later indices; retain `Settings.hud` and its NVS key without consulting it for runtime rendering.

**Step 5: Clear transitions safely**

On portrait prompt exit, clear/invalidate the full sprite once before the pet resumes. On landscape prompt exit, force a full background repaint and render the centered pet. Paint usage bars last. Do not alter approval commands, button handling, BLE, orientation thresholds, clock, menus, or animation cadence.

**Step 6: Verify focused and full firmware behavior**

Run:

```bash
cd firmware
g++ -std=c++17 -Isrc tests/runtime_pet_layout_logic_test.cpp -o /tmp/runtime_pet_layout_logic_test
/tmp/runtime_pet_layout_logic_test
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

Expected: every host test and the StickS3 build exit `0`.

**Step 7: Commit locally without deployment**

```bash
git add firmware/src firmware/tests
git commit -m "feat: show pet-only Codex runtime"
```

Do not flash the device, reinstall the host service, or push the branch. Wait for the user's remaining changes.
