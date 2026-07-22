#pragma once

#include <stdint.h>

struct ScreenOrientationRenderState {
  bool autoSurfaceEligible;
};

struct ScreenOrientationRenderDecision {
  bool entered;
  bool exited;
  bool draw;
};

inline ScreenOrientationRenderDecision screenOrientAutoSurfaceDecision(
  ScreenOrientationRenderState* state,
  bool autoSurfaceEligible,
  bool orientationResolved
) {
  ScreenOrientationRenderDecision decision = {false, false, false};
  if (state == nullptr) return decision;
  decision.entered = autoSurfaceEligible && !state->autoSurfaceEligible;
  decision.exited = !autoSurfaceEligible && state->autoSurfaceEligible;
  decision.draw = autoSurfaceEligible && orientationResolved;
  state->autoSurfaceEligible = autoSurfaceEligible;
  return decision;
}

inline bool screenOrientRuntimeEligible(
  bool normal_display,
  bool menu_open,
  bool settings_open,
  bool reset_open,
  bool codex_active,
  bool codex_waiting,
  bool approval_visible
) {
  return normal_display
      && !menu_open
      && !settings_open
      && !reset_open
      && (codex_active || codex_waiting || approval_visible);
}

inline bool screenOrientRuntimeModeChanged(
  bool previousApprovalVisible,
  bool approvalVisible,
  bool runtimeOrienting
) {
  return runtimeOrienting && previousApprovalVisible != approvalVisible;
}

struct RuntimeLandscapeRenderState {
  bool initialized;
  bool overlayVisible;
  uint32_t lastPetRenderMs;
  uint32_t overlayContentRevision;
  uint32_t overlayTimeRevision;
  uint8_t overlayScrollOffset;
};

struct RuntimeLandscapeRenderDecision {
  bool repaint;
  bool pet;
  bool overlay;
};

inline RuntimeLandscapeRenderDecision runtimeLandscapeSchedule(
  RuntimeLandscapeRenderState* state,
  uint32_t now,
  bool forceRepaint,
  bool overlayVisible,
  uint32_t overlayContentRevision,
  uint32_t overlayTimeRevision,
  uint8_t overlayScrollOffset
) {
  RuntimeLandscapeRenderDecision decision = {false, false, false};
  if (state == nullptr) return decision;

  bool initial = !state->initialized;
  decision.repaint = forceRepaint || initial;
  decision.pet = decision.repaint || (uint32_t)(now - state->lastPetRenderMs) >= 200;
  decision.overlay = decision.repaint
      || initial
      || state->overlayVisible != overlayVisible
      || (overlayVisible && (
          state->overlayContentRevision != overlayContentRevision
          || state->overlayTimeRevision != overlayTimeRevision
          || state->overlayScrollOffset != overlayScrollOffset));

  if (decision.pet) state->lastPetRenderMs = now;
  state->initialized = true;
  state->overlayVisible = overlayVisible;
  state->overlayContentRevision = overlayContentRevision;
  state->overlayTimeRevision = overlayTimeRevision;
  state->overlayScrollOffset = overlayScrollOffset;
  return decision;
}
