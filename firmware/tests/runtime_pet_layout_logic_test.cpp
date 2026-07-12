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
