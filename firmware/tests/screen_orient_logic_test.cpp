#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <string>

#include "clock_orient_logic.h"
#include "screen_orient_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  std::ifstream mainSourceFile("firmware/src/main.cpp");
  std::stringstream mainSourceBuffer;
  mainSourceBuffer << mainSourceFile.rdbuf();
  const std::string mainSource = mainSourceBuffer.str();
  expect_true(!mainSource.empty(), "orientation integration test should load firmware/src/main.cpp");
  expect_true(
    mainSource.find(
      "bool renderSurface = !autoSurfaceAwaitingOrientation;"
    ) != std::string::npos,
    "an unresolved auto surface must gate the entire update and render chain"
  );
  expect_true(
    mainSource.find("screenOrientRuntimeModeChanged(") != std::string::npos &&
      mainSource.find("runtimeSurfaceDecision.entered || runtimeSurfaceModeChanged") !=
        std::string::npos,
    "shared-face and approval transitions must trigger a fresh immediate orientation resolve"
  );

  expect_true(screenOrientRuntimeModeChanged(false, true, true),
              "entering approval from the shared runtime face should restart initial resolve");
  expect_true(screenOrientRuntimeModeChanged(true, false, true),
              "returning from approval to the shared runtime face should restart initial resolve");
  expect_true(!screenOrientRuntimeModeChanged(true, true, true),
              "remaining on the approval surface should not restart resolve every frame");
  expect_true(!screenOrientRuntimeModeChanged(true, false, false),
              "leaving runtime orientation entirely should use the ordinary eligibility exit");

  ScreenOrientationRenderState gate = {};
  ClockOrientationState sideways = {};
  ScreenOrientationRenderDecision gateDecision = screenOrientAutoSurfaceDecision(
    &gate, true, sideways.resolved
  );
  expect_true(gateDecision.entered && !gateDecision.draw,
              "a cold sideways home surface must not draw while its IMU pose is unresolved");
  expect_true(clockOrientResolveInitialForStickS3(&sideways, 0.0f, 0.95f, 0.0f, 0),
              "a clearly sideways home surface should resolve before its first draw");
  gateDecision = screenOrientAutoSurfaceDecision(&gate, true, sideways.resolved);
  expect_true(gateDecision.draw && sideways.orientation == 1,
              "no portrait frame may precede a clearly sideways home frame");
  gateDecision = screenOrientAutoSurfaceDecision(&gate, false, true);
  expect_true(!gateDecision.draw && gateDecision.exited,
              "menus, settings, Wi-Fi, reset, and OTA should make the auto surface ineligible");
  gateDecision = screenOrientAutoSurfaceDecision(&gate, true, false);
  expect_true(gateDecision.entered && !gateDecision.draw,
              "returning from a portrait-only page must resolve before any auto-surface frame");

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

  return 0;
}
