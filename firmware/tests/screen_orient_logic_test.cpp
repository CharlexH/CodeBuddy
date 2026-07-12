#include <stdio.h>
#include <stdlib.h>

#include "clock_orient_logic.h"
#include "screen_orient_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  expect_true(
    screenOrientRuntimeEligible(true, false, false, false, true, false, false),
    "active Codex work in the normal home screen should permit runtime landscape"
  );
  expect_true(
    screenOrientRuntimeEligible(true, false, false, false, false, true, false),
    "waiting Codex work in the normal home screen should permit runtime landscape"
  );
  expect_true(
    screenOrientRuntimeEligible(true, false, false, false, false, false, true),
    "a visible approval prompt should permit runtime landscape"
  );

  expect_true(
    !screenOrientRuntimeEligible(false, false, false, false, true, false, false),
    "non-normal display modes should remain portrait"
  );
  expect_true(
    !screenOrientRuntimeEligible(true, true, false, false, true, false, false),
    "open menus should remain portrait"
  );
  expect_true(
    !screenOrientRuntimeEligible(true, false, true, false, true, false, false),
    "open settings should remain portrait"
  );
  expect_true(
    !screenOrientRuntimeEligible(true, false, false, true, true, false, false),
    "open reset confirmation should remain portrait"
  );
  expect_true(
    !screenOrientRuntimeEligible(true, false, false, false, false, false, false),
    "idle standby orientation should remain owned by the charging-clock policy"
  );

  expect_true(
    screenOrientRuntimeEligible(true, false, false, false, true, false, false) ==
      screenOrientRuntimeEligible(true, false, false, false, false, true, false),
    "active and waiting shared faces should use the same runtime orientation eligibility"
  );

  RuntimeLandscapeRenderState render = {};
  RuntimeLandscapeRenderDecision decision = runtimeLandscapeSchedule(
    &render, 0, true, true, 1, 0, 0
  );
  expect_true(decision.repaint && decision.pet && decision.overlay,
              "entering landscape should repaint pet and overlay immediately");

  decision = runtimeLandscapeSchedule(&render, 16, false, true, 1, 0, 0);
  expect_true(!decision.repaint && !decision.pet && !decision.overlay,
              "unchanged landscape frame should not redraw direct LCD content");

  decision = runtimeLandscapeSchedule(&render, 199, false, true, 1, 0, 0);
  expect_true(!decision.pet, "pet should wait for the 5fps render interval");
  decision = runtimeLandscapeSchedule(&render, 200, false, true, 1, 0, 0);
  expect_true(decision.pet && !decision.overlay,
              "pet should redraw at the 5fps render interval without rewriting the overlay");

  decision = runtimeLandscapeSchedule(&render, 216, false, true, 2, 0, 0);
  expect_true(!decision.pet && decision.overlay,
              "prompt or HUD content changes should redraw the overlay immediately");
  decision = runtimeLandscapeSchedule(&render, 232, false, true, 2, 1, 0);
  expect_true(!decision.pet && decision.overlay,
              "prompt time changes should redraw the overlay without a pet redraw");
  decision = runtimeLandscapeSchedule(&render, 248, false, true, 2, 1, 1);
  expect_true(!decision.pet && decision.overlay,
              "scroll changes should redraw the overlay without a pet redraw");
  decision = runtimeLandscapeSchedule(&render, 264, false, false, 2, 1, 1);
  expect_true(decision.overlay,
              "hiding a prompt or HUD should clear the prior overlay immediately");

  RuntimeLandscapeRenderState petOnly = {};
  decision = runtimeLandscapeSchedule(&petOnly, 0, true, false, 0, 0, 0);
  expect_true(decision.repaint && decision.pet && decision.overlay,
              "entering pet-only landscape should paint the complete surface once");
  decision = runtimeLandscapeSchedule(&petOnly, 16, false, false, 99, 7, 3);
  expect_true(!decision.repaint && !decision.pet && !decision.overlay,
              "transcript revisions must not repaint a pet-only landscape surface");

  decision = runtimeLandscapeSchedule(&render, 280, true, false, 2, 1, 1);
  expect_true(decision.repaint && decision.pet && decision.overlay,
              "an orientation repaint should redraw all direct LCD layers");

  uint8_t orient = 0;
  int8_t orientFrames = 0;
  int8_t swapFrames = 0;
  for (int i = 0; i < 14; ++i) {
    clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, 0.95f, 0.0f, 0);
  }
  expect_true(orient == 0, "runtime landscape should enter only after 15 side frames");
  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, 0.95f, 0.0f, 0);
  expect_true(orient == 1, "runtime landscape should enter on the 15th side frame");

  orientFrames = 0;
  for (int i = 0; i < 7; ++i) {
    clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.95f, 0.0f, 0.0f, 0);
  }
  expect_true(orient == 1, "runtime landscape should keep its orientation through seven portrait frames");
  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.95f, 0.0f, 0.0f, 0);
  expect_true(orient == 0, "runtime landscape should exit on the eighth portrait frame");

  orientFrames = 0;
  swapFrames = 0;
  for (int i = 0; i < 15; ++i) {
    clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, 0.95f, 0.0f, 0);
  }
  for (int i = 0; i < 7; ++i) {
    clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, -0.95f, 0.0f, 0);
  }
  expect_true(orient == 1, "runtime landscape side swap should wait for eight stable frames");
  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, -0.95f, 0.0f, 0);
  expect_true(orient == 3, "runtime landscape side swap should occur on the eighth stable frame");

  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, 0.0f, 0.0f, 1);
  expect_true(orient == 0, "forced portrait setting should override runtime landscape immediately");
  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, 0.95f, 0.0f, 2);
  expect_true(orient == 1, "forced landscape setting should select the positive side immediately");
  clockOrientUpdateForStickS3(&orient, &orientFrames, &swapFrames, 0.0f, -0.95f, 0.0f, 2);
  expect_true(orient == 3, "forced landscape setting should follow the negative side immediately");

  return 0;
}
