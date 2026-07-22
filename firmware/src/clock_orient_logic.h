#pragma once

#include <math.h>
#include <stdint.h>

// Auto-oriented surfaces distinguish an unclassified entry pose from a
// deliberate portrait choice.  That distinction prevents a cold sideways
// surface from drawing a speculative portrait frame while its IMU settles.
struct ClockOrientationState {
  uint8_t orientation;
  uint8_t stableOrientation;
  int8_t orientFrames;
  int8_t swapFrames;
  bool resolved;
  bool hasStableOrientation;
};

inline void clockOrientRemember(ClockOrientationState* state, uint8_t orientation) {
  if (state == nullptr) return;
  state->orientation = orientation;
  state->stableOrientation = orientation;
  state->hasStableOrientation = true;
  state->resolved = true;
}

inline void clockOrientBeginAutoSurface(ClockOrientationState* state) {
  if (state == nullptr) return;
  state->resolved = false;
  state->orientFrames = 0;
  state->swapFrames = 0;
  if (state->hasStableOrientation) state->orientation = state->stableOrientation;
}

inline bool clockOrientResolveInitialForStickS3(
  ClockOrientationState* state,
  float ax,
  float ay,
  float az,
  uint8_t lock
) {
  if (state == nullptr) return false;
  const float primary = ay;
  const float secondary = ax;

  if (lock == 1) {
    clockOrientRemember(state, 0);
    state->orientFrames = 0;
    state->swapFrames = 0;
    return true;
  }
  if (lock == 2) {
    uint8_t orientation = state->orientation;
    if (orientation == 0) orientation = primary >= 0.0f ? 1 : 3;
    if (primary > 0.5f) orientation = 1;
    else if (primary < -0.5f) orientation = 3;
    clockOrientRemember(state, orientation);
    state->orientFrames = 0;
    state->swapFrames = 0;
    return true;
  }

  const bool strongLandscape = fabsf(primary) > 0.7f &&
    fabsf(secondary) < 0.5f && fabsf(az) < 0.5f;
  const bool strongPortrait = fabsf(secondary) > 0.7f &&
    fabsf(primary) < 0.5f && fabsf(az) < 0.5f;
  if (strongLandscape) {
    clockOrientRemember(state, primary > 0.0f ? 1 : 3);
  } else if (strongPortrait) {
    clockOrientRemember(state, 0);
  } else if (state->hasStableOrientation) {
    state->orientation = state->stableOrientation;
    state->resolved = true;
  } else {
    state->resolved = false;
    return false;
  }

  state->orientFrames = 0;
  state->swapFrames = 0;
  return true;
}

inline void clockOrientUpdateStateForStickS3(
  ClockOrientationState* state,
  float ax,
  float ay,
  float az,
  uint8_t lock
) {
  if (state == nullptr) return;
  if (!state->resolved || lock != 0) {
    clockOrientResolveInitialForStickS3(state, ax, ay, az, lock);
    return;
  }

  const float primary = ay;
  const float secondary = ax;
  bool side = state->orientation == 0
    ? fabsf(primary) > 0.7f && fabsf(secondary) < 0.5f && fabsf(az) < 0.5f
    : fabsf(primary) > 0.4f;

  if (side) {
    if (state->orientFrames < 20) state->orientFrames++;
  } else {
    if (state->orientFrames > -10) state->orientFrames--;
  }

  if (state->orientation == 0 && state->orientFrames >= 15) {
    clockOrientRemember(state, primary > 0.0f ? 1 : 3);
  } else if (state->orientation != 0 && state->orientFrames <= -8) {
    clockOrientRemember(state, 0);
  } else if (state->orientation != 0 && side) {
    const uint8_t want = primary > 0.0f ? 1 : 3;
    if (want != state->orientation) {
      if (++state->swapFrames >= 8) {
        clockOrientRemember(state, want);
        state->swapFrames = 0;
      }
    } else {
      state->swapFrames = 0;
    }
  }
}

// Existing callers retain the old scalar API.  New auto-surface code should
// retain ClockOrientationState so remembered orientation survives page changes.
inline void clockOrientUpdateForStickS3(
  uint8_t* clock_orient,
  int8_t* orient_frames,
  int8_t* swap_frames,
  float ax,
  float ay,
  float az,
  uint8_t lock
) {
  if (clock_orient == nullptr || orient_frames == nullptr || swap_frames == nullptr) return;
  ClockOrientationState state = {
    *clock_orient, *clock_orient, *orient_frames, *swap_frames, true, true
  };
  clockOrientUpdateStateForStickS3(&state, ax, ay, az, lock);
  *clock_orient = state.orientation;
  *orient_frames = state.orientFrames;
  *swap_frames = state.swapFrames;
}
