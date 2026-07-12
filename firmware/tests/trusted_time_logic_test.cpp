#include <stdio.h>
#include <stdlib.h>

#include "trusted_time_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}

int main() {
  TrustedTimeState state = trustedTimeInitial();
  expect_true(!trustedTimeAcceptHost(&state, 1710000000, 28800, false, 100),
              "numeric time over open BLE must not become trusted");
  expect_true(!trustedTimeFresh(state, 100), "open BLE time must remain untrusted");
  expect_true(trustedTimeAcceptHost(&state, 1710000000, 28800, true, 200),
              "authenticated host time should become trusted");
  expect_true(state.source == TRUSTED_TIME_SECURE_HOST && trustedTimeFresh(state, 200),
              "secure host source and freshness should be recorded");
  expect_true(!trustedTimeFresh(state, 200 + TRUSTED_TIME_MAX_AGE_MS + 1),
              "trusted time must expire");

  state = trustedTimeInitial();
  expect_true(!trustedTimeAcceptSntp(&state, 1710000000, false, 300),
              "SNTP request alone must not establish trust");
  expect_true(trustedTimeAcceptSntp(&state, 1710000000, true, 400),
              "completed SNTP sync should establish trust");
  expect_true(state.source == TRUSTED_TIME_SNTP, "SNTP source should be recorded");
  expect_true(!trustedTimeInputSane(1, 0), "ancient epoch should be rejected");
  expect_true(!trustedTimeInputSane(5000000000LL, 0), "absurd future epoch should be rejected");
  expect_true(!trustedTimeInputSane(1710000000, 15 * 3600), "absurd timezone should be rejected");
  expect_true(trustedTimeInputSane(1710000000, -12 * 3600), "sane epoch/timezone should pass");
  return 0;
}
