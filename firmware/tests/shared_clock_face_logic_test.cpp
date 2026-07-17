#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "shared_clock_face_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  const SharedClockFaceLayout portrait = sharedClockFaceLayout(false);
  expect_true(portrait.screenWidth == 135 && portrait.screenHeight == 240,
              "portrait shared face should use the full 135x240 display");
  expect_true(portrait.pet.x == 0 && portrait.pet.y == 0 &&
                  portrait.pet.width == 135 && portrait.pet.height == 90,
              "portrait shared face should reserve the top 135x90 for the compact pet");
  expect_true(!portrait.pet.useLocalSurface && portrait.pet.asciiScale == 1 &&
                  portrait.pet.asciiYOffset == 0,
              "portrait pet rendering should retain its existing direct 1x placement");
  expect_true(portrait.time.primary.x == 19 && portrait.time.primary.y == 166 &&
                  portrait.time.primary.width == 60 && portrait.time.primary.height == 16,
              "portrait HH:MM should start the centered 96-pixel time line at y 174");
  expect_true(portrait.time.seconds.x == 79 && portrait.time.seconds.y == 166 &&
                  portrait.time.seconds.width == 36 && portrait.time.seconds.height == 16,
              "portrait :SS should continue on the same line at the same size");
  expect_true(portrait.time.textSize == 2 && portrait.time.centerY == 174,
              "portrait time should use text size 2 centered at y 174");
  expect_true(portrait.time.showSeconds,
              "portrait time should retain its existing seconds segment");
  expect_true(portrait.time.primary.role == SHARED_CLOCK_TEXT_PRIMARY &&
                  portrait.time.seconds.role == SHARED_CLOCK_TEXT_DIM,
              "portrait seconds should use the dim role while staying the same size");
  expect_true(portrait.date.mode == SHARED_CLOCK_DATE_SINGLE_LINE &&
                  portrait.date.centerX == 67 && portrait.date.centerY == 202 &&
                  portrait.date.textSize == 1,
              "portrait date should remain centered around y 202 at text size 1");
  expect_true(portrait.meterY == 224 && portrait.meterFootprint == 16,
              "portrait meters should occupy the final 16 pixels from y 224");
  expect_true(!portrait.status.visible,
              "portrait should not reserve a task-status dashboard");

  const SharedClockFaceLayout landscape = sharedClockFaceLayout(true);
  expect_true(landscape.screenWidth == 240 && landscape.screenHeight == 135,
              "landscape shared face should use the full 240x135 display");
  expect_true(landscape.pet.x == 0 && landscape.pet.y == 0 &&
                  landscape.pet.width == 120 && landscape.pet.height == 58,
              "landscape shared face should reserve the left 120x58 region for the pet");
  expect_true(landscape.pet.x + landscape.pet.width / 2 == 60 &&
                  landscape.pet.y + landscape.pet.height / 2 == 29,
              "landscape compact pet should be centered at 60,29");
  expect_true(landscape.status.visible && landscape.status.x == 120 &&
                  landscape.status.y == 0 && landscape.status.width == 120 &&
                  landscape.status.height == 58 && landscape.status.columnWidth == 40,
              "landscape status dashboard should occupy the right 120x58 region");
  expect_true(landscape.pet.useLocalSurface && landscape.pet.asciiScale == 1 &&
                  landscape.pet.asciiYOffset == -13,
              "landscape pet rendering should clip through its local surface and lift 1x ASCII into it");
  expect_true(landscape.pet.asciiYOffset + 30 + 1 + 4 * 8 + 8 <=
                  landscape.pet.height,
              "the lowest five-line ASCII frame should fit inside the 58-pixel local surface");
  expect_true(!sharedClockPetLocalSurfaceNeedsClear(false, false),
              "an unchanged compact frame should retain the local pet surface");
  expect_true(sharedClockPetLocalSurfaceNeedsClear(true, false) &&
                  sharedClockPetLocalSurfaceNeedsClear(false, true),
              "ASCII frames and full repaints should clear the local pet surface");
  expect_true(landscape.time.primary.x == 8 && landscape.time.primary.y == 77 &&
                  landscape.time.primary.width == 120 && landscape.time.primary.height == 32,
              "landscape HH:MM should use a 120x32 line starting at x 8 inside the clock row");
  expect_true(landscape.time.textSize == 4 && landscape.time.centerY == 93,
              "landscape HH:MM should use text size 4 centered at y 93");
  expect_true(landscape.time.showSeconds &&
                  landscape.time.seconds.x == 128 &&
                  landscape.time.seconds.y == 77 &&
                  landscape.time.seconds.width == 72 &&
                  landscape.time.seconds.height == 32,
              "landscape :SS should continue beside HH:MM at text size 4");
  expect_true(landscape.time.seconds.role == SHARED_CLOCK_TEXT_DIM,
              "landscape seconds should stay visually secondary");
  expect_true(landscape.time.primary.role == SHARED_CLOCK_TEXT_PRIMARY,
              "landscape HH:MM should retain the primary text role");
  expect_true(landscape.date.mode == SHARED_CLOCK_DATE_STACKED_NUMERIC &&
                  landscape.date.textSize == 2,
              "landscape date should use stacked numeric month and day at text size 2");
  expect_true(landscape.date.month.x == 208 && landscape.date.month.y == 74 &&
                  landscape.date.month.width == 24 && landscape.date.month.height == 16,
              "landscape month should be a right-aligned two-digit line near the top of the clock row");
  expect_true(landscape.date.day.x == 208 && landscape.date.day.y == 94 &&
                  landscape.date.day.width == 24 && landscape.date.day.height == 16,
              "landscape day should be a right-aligned two-digit line below the month");
  expect_true(landscape.date.day.y -
                  (landscape.date.month.y + landscape.date.month.height) == 4,
              "landscape month and day should use a compact four-pixel gap");
  expect_true(landscape.pet.y + landscape.pet.height <= 58 &&
                  landscape.time.primary.y >= 60 &&
                  landscape.time.primary.y + landscape.time.primary.height <= 111 &&
                  landscape.date.month.y >= 60 &&
                  landscape.date.day.y + landscape.date.day.height <= 111,
              "landscape pet clears and clock/date text should stay in their non-overlapping regions");
  expect_true(landscape.meterY == 111 && landscape.meterFootprint == 24,
              "landscape meter footprint should occupy the final 24 pixels from y 111");

  SharedClockFaceContext context = {};
  context.normalDisplay = true;
  expect_true(sharedClockFaceSelected(context, SHARED_CLOCK_IDLE),
              "normal idle standby should select the shared clock face");
  expect_true(sharedClockFaceSelected(context, SHARED_CLOCK_ACTIVE),
              "normal active Codex work should select the same shared clock face");
  expect_true(sharedClockFaceSelected(context, SHARED_CLOCK_WAITING),
              "normal waiting Codex work should select the same shared clock face");

  context.normalDisplay = false;
  expect_true(!sharedClockFaceSelected(context, SHARED_CLOCK_IDLE),
              "a non-normal display mode should exclude the shared face");
  context.normalDisplay = true;

  bool* exclusions[] = {
    &context.menuVisible,
    &context.settingsVisible,
    &context.resetVisible,
    &context.passkeyVisible,
    &context.promptVisible,
    &context.functionalOverrideVisible,
    &context.otaProgressVisible,
  };
  const char* exclusionMessages[] = {
    "menu should exclude the shared face",
    "settings should exclude the shared face",
    "reset confirmation should exclude the shared face",
    "passkey entry should exclude the shared face",
    "approval prompt should exclude the shared face",
    "functional override should exclude the shared face",
    "OTA progress should exclude the shared face",
  };
  for (uint8_t i = 0; i < sizeof(exclusions) / sizeof(exclusions[0]); ++i) {
    *exclusions[i] = true;
    expect_true(!sharedClockFaceSelected(context, SHARED_CLOCK_ACTIVE), exclusionMessages[i]);
    *exclusions[i] = false;
  }

  expect_true(SHARED_CLOCK_PET_FRAME_INTERVAL_MS == 200,
              "shared face pet animation should use a 200ms cadence");
  expect_true(!sharedClockPetFrameDue(199, 200),
              "pet animation should not render before its next 200ms deadline");
  expect_true(sharedClockPetFrameDue(200, 200),
              "pet animation should render at its next 200ms deadline");
  expect_true(sharedClockPetFrameDue(10, UINT32_MAX - 5),
              "pet animation deadline checks should be safe across millis wraparound");

  SharedClockFaceRenderInput renderInput = {};
  renderInput.firstEntry = true;
  SharedClockFaceRenderDecision decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(decision.fullRepaint && decision.clearSurface && decision.drawTime &&
                  decision.drawDate && decision.drawPet && decision.drawMeters,
              "first shared-face entry should fully repaint every layer");

  renderInput = {};
  renderInput.orientationChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(decision.fullRepaint && decision.drawTime && decision.drawDate &&
                  decision.drawPet && decision.drawMeters,
              "orientation changes should fully repaint every layer");

  renderInput = {};
  renderInput.fullRepaintRequested = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(decision.fullRepaint && decision.clearSurface,
              "explicit full repaint requests should clear the shared face");

  renderInput = {};
  renderInput.secondChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && decision.drawTime && !decision.drawDate &&
                  !decision.drawPet && !decision.drawMeters,
              "a second tick should update only the one-line time text");

  renderInput = {};
  renderInput.dateChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && !decision.drawTime && decision.drawDate,
              "a date change should update the retained date without clearing the face");

  renderInput = {};
  renderInput.petFrameDue = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && decision.drawPet && !decision.drawTime &&
                  !decision.drawDate && !decision.drawMeters,
              "a 200ms pet frame should update only the compact pet layer");

  renderInput = {};
  renderInput.metersChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && decision.drawMeters,
              "changed usage values should redraw the meter layer");
  renderInput = {};
  renderInput.forceMeters = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && decision.drawMeters,
              "a meter force request should redraw the meter layer");

  renderInput = {};
  renderInput.activitySurfaceChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && !decision.clearSurface && !decision.drawTime &&
                  !decision.drawDate && !decision.drawPet && !decision.drawMeters,
              "idle-active surface switches should not imply a different layout or repaint");

  renderInput = {};
  renderInput.promptExited = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(decision.fullRepaint && decision.clearSurface && decision.drawTime &&
                  decision.drawDate && decision.drawPet && decision.drawMeters,
              "approval prompt exit should force a clean full shared-face repaint");

  const SharedClockTimePolicy validTime = sharedClockTimePolicy(true);
  expect_true(!validTime.usePlaceholder,
              "valid host-synced time should render its time and date values");
  const SharedClockTimePolicy invalidTime = sharedClockTimePolicy(false);
  expect_true(invalidTime.usePlaceholder,
              "invalid time should select the clock-face placeholder text");

  return 0;
}
