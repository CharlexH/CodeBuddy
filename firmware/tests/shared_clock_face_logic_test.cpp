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
  expect_true(portrait.time.primary.x == 36 && portrait.time.primary.y == 166 &&
                  portrait.time.primary.width == 38 && portrait.time.primary.height == 16,
              "portrait HH:MM should start the centered custom-font time line at y 174");
  expect_true(portrait.time.seconds.x == 75 && portrait.time.seconds.y == 166 &&
                  portrait.time.seconds.width == 23 && portrait.time.seconds.height == 16,
              "portrait :SS should continue on the same line at the same size");
  expect_true(portrait.time.primaryTextSize == 0.75f &&
                  portrait.time.secondsTextSize == 0.75f &&
                  portrait.time.centerY == 174,
              "portrait time should retain its natural aspect ratio and stay centered");
  expect_true(portrait.time.showSeconds,
              "portrait time should retain its existing seconds segment");
  expect_true(portrait.time.primary.role == SHARED_CLOCK_TEXT_PRIMARY &&
                  portrait.time.seconds.role == SHARED_CLOCK_TEXT_DIM,
              "portrait seconds should use the dim role while staying the same size");
  expect_true(portrait.date.mode == SHARED_CLOCK_DATE_SINGLE_LINE &&
                  portrait.date.centerX == 67 && portrait.date.centerY == 202 &&
                  portrait.date.monthTextSize == 0.38f &&
                  portrait.date.dayTextSize == 0.38f,
              "portrait date should fit the custom font's line height");
  expect_true(portrait.meterY == 224 && portrait.meterFootprint == 16,
              "portrait meters should occupy the final 16 pixels from y 224");
  expect_true(!portrait.status.visible,
              "portrait should not reserve a task-status dashboard");

  const SharedClockFaceLayout landscape = sharedClockFaceLayout(true);
  expect_true(landscape.screenWidth == 240 && landscape.screenHeight == 135,
              "landscape shared face should use the full 240x135 display");
  expect_true(landscape.status.visible,
              "landscape should keep count-change scheduling enabled for the dashboard cards");
  expect_true(landscape.meterY == 109 && landscape.meterFootprint == 26,
              "landscape meter should match the Figma footer's four-pixel bottom inset");

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
                  decision.drawDate && decision.drawPet && !decision.drawStatus &&
                  decision.drawMeters,
              "first shared-face entry should fully repaint every layer");

  renderInput = {};
  renderInput.statusVisible = true;
  renderInput.statusChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.fullRepaint && decision.drawStatus && !decision.clearSurface &&
                  !decision.drawTime && !decision.drawDate && !decision.drawPet &&
                  !decision.drawMeters,
              "a landscape count change should redraw only the status dashboard");

  renderInput = {};
  renderInput.statusChanged = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(!decision.drawStatus,
              "portrait should ignore task-status count changes");

  renderInput = {};
  renderInput.orientationChanged = true;
  renderInput.statusVisible = true;
  decision = sharedClockFaceRenderDecision(renderInput);
  expect_true(decision.fullRepaint && decision.drawTime && decision.drawDate &&
                  decision.drawPet && decision.drawStatus && decision.drawMeters,
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
