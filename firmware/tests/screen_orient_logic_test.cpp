#include <stdio.h>
#include <stdlib.h>

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
    "idle home screen should remain portrait"
  );

  return 0;
}
