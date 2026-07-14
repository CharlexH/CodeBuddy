#pragma once

#include <stdint.h>

struct CompletionChimeState {
  bool hasBaseline;
  uint32_t lastSequence;
};

inline bool completionChimeObserve(
  CompletionChimeState* state,
  bool hasSequence,
  uint32_t sequence
) {
  if (!state || !hasSequence) return false;
  if (!state->hasBaseline) {
    state->hasBaseline = true;
    state->lastSequence = sequence;
    return false;
  }
  if (state->lastSequence == sequence) return false;
  state->lastSequence = sequence;
  return true;
}
