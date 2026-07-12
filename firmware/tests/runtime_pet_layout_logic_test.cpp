#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime_pet_layout_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  const RuntimePetLayout portrait = runtimePetLayout(false);
  expect_true(portrait.viewportWidth == 135, "portrait viewport width should fill the display");
  expect_true(portrait.viewportHeight == 224, "portrait viewport should reserve 16 pixels for meters");
  expect_true(portrait.centerX == 67, "portrait pet should be horizontally centered");
  expect_true(portrait.centerY == 112, "portrait pet should be vertically centered above meters");
  expect_true(portrait.asciiScale == 2, "portrait ASCII pet should use scale 2");
  expect_true(portrait.asciiYOffset == 30, "portrait ASCII pet should use the portrait vertical offset");

  const RuntimePetLayout landscape = runtimePetLayout(true);
  expect_true(landscape.viewportWidth == 240, "landscape viewport width should fill the display");
  expect_true(landscape.viewportHeight == 119, "landscape viewport should reserve 16 pixels for meters");
  expect_true(landscape.centerX == 120, "landscape pet should be horizontally centered");
  expect_true(landscape.centerY == 59, "landscape pet should be vertically centered above meters");
  expect_true(landscape.asciiScale == 1, "landscape ASCII pet should use scale 1");
  expect_true(landscape.asciiYOffset == 18, "landscape ASCII pet should use the landscape vertical offset");

  const RuntimePetClearRect portraitClear = runtimePetClearRect(false);
  expect_true(portraitClear.x == 0 && portraitClear.y == 0,
              "portrait ASCII frames should clear particles above the visual offset");
  expect_true(portraitClear.width == 135 && portraitClear.height == 224,
              "portrait ASCII frames should clear the full pet viewport without the meter footprint");
  const RuntimePetClearRect landscapeClear = runtimePetClearRect(true);
  expect_true(landscapeClear.x == 0 && landscapeClear.y == 0,
              "landscape ASCII frames should clear particles above the visual offset");
  expect_true(landscapeClear.width == 240 && landscapeClear.height == 119,
              "landscape ASCII frames should clear the full pet viewport without the meter footprint");

  expect_true(runtimeGifScaleDivisor(false, 135, 224) == 1,
              "portrait runtime GIFs should stay at native scale");
  expect_true(runtimeGifScaleDivisor(true, 236, 119) == 1,
              "a landscape GIF that fits the safe viewport should stay at native scale");
  expect_true(runtimeGifScaleDivisor(true, 237, 119) == 2,
              "a landscape GIF wider than the safe viewport should use half scale");
  expect_true(runtimeGifScaleDivisor(true, 236, 120) == 2,
              "a landscape GIF taller than the runtime viewport should use half scale");

  const RuntimeGifPlacement portraitGif = runtimeGifPlacement(false, 100, 80);
  expect_true(portraitGif.x == 17 && portraitGif.y == 72 && portraitGif.scaleDivisor == 1,
              "portrait GIFs should be natively centered in the runtime viewport");
  const RuntimeGifPlacement landscapeGif = runtimeGifPlacement(true, 100, 80);
  expect_true(landscapeGif.x == 70 && landscapeGif.y == 19 && landscapeGif.scaleDivisor == 1,
              "fitting landscape GIFs should be natively centered");
  const RuntimeGifPlacement scaledLandscapeGif = runtimeGifPlacement(true, 240, 120);
  expect_true(scaledLandscapeGif.x == 60 && scaledLandscapeGif.y == 29
                  && scaledLandscapeGif.scaleDivisor == 2,
              "oversized landscape GIFs should be half-scaled and centered");

  RuntimeDirectFrameDecision directFrame = runtimeDirectFrameDecision(
    false, true, 0, 100, 100
  );
  expect_true(directFrame.render && directFrame.clear,
              "a due direct GIF frame should clear the viewport before rendering");
  directFrame = runtimeDirectFrameDecision(false, true, 0, 99, 100);
  expect_true(!directFrame.render && !directFrame.clear,
              "a direct GIF frame that is not due should not clear the viewport");
  directFrame = runtimeDirectFrameDecision(true, false, 2, 100, 100);
  expect_true(directFrame.render && directFrame.clear,
              "a due text-mode frame should render without an open GIF");
  directFrame = runtimeDirectFrameDecision(true, false, 0, 100, 100);
  expect_true(!directFrame.render && !directFrame.clear,
              "a text-mode state without frames should leave the viewport unchanged");
  directFrame = runtimeDirectFrameDecision(false, false, 0, 100, 100);
  expect_true(!directFrame.render && !directFrame.clear,
              "an unavailable direct character should leave the viewport unchanged");

  const RuntimeTextPlacement landscapeText = runtimeTextPlacement(5, 120, 59);
  expect_true(landscapeText.x == 90 && landscapeText.y == 51,
              "landscape text-mode frames should be centered at the runtime pet center");

  expect_true(!runtimeStatusOverlayVisible(false), "normal runtime should hide the status overlay");
  expect_true(runtimeStatusOverlayVisible(true), "approval prompts should show the status overlay");

  expect_true(!runtimeNeedsFullRepaintOnPromptExit(false, false),
              "normal runtime without a prompt transition should not force a full repaint");
  expect_true(!runtimeNeedsFullRepaintOnPromptExit(false, true),
              "prompt entry should not use the prompt-exit repaint policy");
  expect_true(!runtimeNeedsFullRepaintOnPromptExit(true, true),
              "an active prompt should not use the prompt-exit repaint policy");
  expect_true(runtimeNeedsFullRepaintOnPromptExit(true, false),
              "prompt exit should force a full repaint");

  return 0;
}
