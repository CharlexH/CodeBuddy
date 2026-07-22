#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "clock_orient_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  ClockOrientationState orient = {};

  expect_true(
    clockOrientResolveInitialForStickS3(&orient, 0.95f, 0.0f, 0.0f, 0),
    "a strong upright cold-start pose should resolve immediately"
  );
  expect_true(orient.resolved && orient.orientation == 0 &&
                  orient.stableOrientation == 0,
              "a strong upright pose should select and remember portrait");

  orient = {};
  expect_true(
    clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.95f, 0.0f, 0),
    "a strong positive sideways cold-start pose should resolve immediately"
  );
  expect_true(orient.resolved && orient.orientation == 1 &&
                  orient.stableOrientation == 1,
              "a strong positive sideways pose should select landscape rotation 1");

  orient = {};
  expect_true(
    clockOrientResolveInitialForStickS3(&orient, 0.0f, -0.95f, 0.0f, 0),
    "a strong negative sideways cold-start pose should resolve immediately"
  );
  expect_true(orient.resolved && orient.orientation == 3 &&
                  orient.stableOrientation == 3,
              "a strong negative sideways pose should select landscape rotation 3");

  orient = {};
  expect_true(
    !clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.0f, 0.0f, 0),
    "an ambiguous cold-start pose should remain unresolved without history"
  );
  expect_true(!orient.resolved,
              "an ambiguous cold-start pose must not speculate a portrait frame");

  orient.orientation = 3;
  orient.stableOrientation = 3;
  orient.hasStableOrientation = true;
  clockOrientBeginAutoSurface(&orient);
  expect_true(
    clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.0f, 0.0f, 0),
    "an ambiguous return should resolve from remembered auto orientation"
  );
  expect_true(orient.resolved && orient.orientation == 3,
              "an ambiguous return must retain the remembered landscape side");

  orient = {};
  expect_true(clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.0f, 0.0f, 1),
              "forced portrait should bypass the unresolved state");
  expect_true(orient.orientation == 0 && orient.resolved,
              "forced portrait should resolve to portrait immediately");
  orient = {};
  expect_true(clockOrientResolveInitialForStickS3(&orient, 0.0f, -0.95f, 0.0f, 2),
              "forced landscape should bypass the unresolved state");
  expect_true(orient.orientation == 3 && orient.resolved,
              "forced landscape should select the signed landscape side immediately");

  orient = {};
  clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.95f, 0.0f, 0);
  for (int i = 0; i < 7; ++i) {
    clockOrientUpdateStateForStickS3(&orient, 0.95f, 0.0f, 0.0f, 0);
  }
  expect_true(orient.orientation == 1,
              "post-resolution hysteresis should retain landscape through seven portrait frames");
  clockOrientUpdateStateForStickS3(&orient, 0.95f, 0.0f, 0.0f, 0);
  expect_true(orient.orientation == 0,
              "post-resolution hysteresis should return to portrait on the eighth frame");

  clockOrientResolveInitialForStickS3(&orient, 0.0f, 0.95f, 0.0f, 0);
  for (int i = 0; i < 7; ++i) {
    clockOrientUpdateStateForStickS3(&orient, 0.0f, -0.95f, 0.0f, 0);
  }
  expect_true(orient.orientation == 1,
              "post-resolution landscape-side swaps should retain their side through seven frames");
  clockOrientUpdateStateForStickS3(&orient, 0.0f, -0.95f, 0.0f, 0);
  expect_true(orient.orientation == 3,
              "post-resolution landscape-side swaps should occur on the eighth frame");

  orient = {};
  clockOrientResolveInitialForStickS3(&orient, 0.0f, -0.95f, 0.0f, 2);
  clockOrientUpdateStateForStickS3(&orient, 0.0f, 0.01f, 0.0f, 2);
  expect_true(orient.orientation == 3,
              "forced landscape should ignore near-zero sign noise and retain its current side");
  clockOrientUpdateStateForStickS3(&orient, 0.0f, 0.49f, 0.0f, 2);
  expect_true(orient.orientation == 3,
              "forced landscape should retain its current side below the swap threshold");
  clockOrientUpdateStateForStickS3(&orient, 0.0f, 0.51f, 0.0f, 2);
  expect_true(orient.orientation == 1,
              "forced landscape should swap sides only past the positive threshold");

  return 0;
}
