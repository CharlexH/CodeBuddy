#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "completion_chime_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  CompletionChimeState state = {};

  expect_true(
    !completionChimeObserve(&state, false, 0),
    "a missing sequence must not establish a baseline or play"
  );
  expect_true(!state.hasBaseline, "a missing sequence must leave the state uninitialized");

  expect_true(
    !completionChimeObserve(&state, true, 41),
    "the first valid sequence must establish a silent baseline"
  );
  expect_true(state.hasBaseline && state.lastSequence == 41,
              "the first valid sequence must be retained as the baseline");

  expect_true(
    !completionChimeObserve(&state, true, 41),
    "a duplicate sequence must not play"
  );
  expect_true(state.lastSequence == 41, "a duplicate must preserve the baseline");

  expect_true(
    completionChimeObserve(&state, true, 42),
    "a changed sequence must play exactly once"
  );
  expect_true(state.lastSequence == 42, "a changed sequence must become the new baseline");
  expect_true(
    !completionChimeObserve(&state, true, 42),
    "the new sequence must not replay on the next observation"
  );

  expect_true(
    !completionChimeObserve(&state, false, 0),
    "an invalid or missing field after a baseline must not be treated as zero"
  );
  expect_true(state.hasBaseline && state.lastSequence == 42,
              "an invalid or missing field must preserve the prior sequence");

  CompletionChimeState wrapState = {true, UINT32_MAX};
  expect_true(
    completionChimeObserve(&wrapState, true, 0),
    "uint32 wraparound must count as a sequence change"
  );
  expect_true(wrapState.lastSequence == 0,
              "the wrapped sequence must become the new baseline");
  expect_true(
    !completionChimeObserve(&wrapState, true, 0),
    "the wrapped sequence must still deduplicate"
  );

  return 0;
}
