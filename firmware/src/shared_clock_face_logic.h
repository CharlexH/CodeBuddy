#pragma once

#include <stdint.h>

enum SharedClockTextRole : uint8_t {
  SHARED_CLOCK_TEXT_PRIMARY,
  SHARED_CLOCK_TEXT_DIM,
};

enum SharedClockActivity : uint8_t {
  SHARED_CLOCK_IDLE,
  SHARED_CLOCK_ACTIVE,
  SHARED_CLOCK_WAITING,
};

struct SharedClockRect {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
};

struct SharedClockTextRect {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  SharedClockTextRole role;
};

struct SharedClockTimeLayout {
  SharedClockTextRect primary;
  SharedClockTextRect seconds;
  int16_t centerY;
  uint8_t textSize;
};

struct SharedClockDateLayout {
  int16_t centerX;
  int16_t centerY;
  uint8_t textSize;
};

struct SharedClockFaceLayout {
  uint16_t screenWidth;
  uint16_t screenHeight;
  SharedClockRect pet;
  SharedClockTimeLayout time;
  SharedClockDateLayout date;
  uint16_t meterY;
  uint8_t meterFootprint;
};

struct SharedClockFaceContext {
  bool normalDisplay;
  bool menuVisible;
  bool settingsVisible;
  bool resetVisible;
  bool passkeyVisible;
  bool promptVisible;
  bool functionalOverrideVisible;
  bool otaProgressVisible;
};

struct SharedClockFaceRenderInput {
  bool firstEntry;
  bool orientationChanged;
  bool fullRepaintRequested;
  bool secondChanged;
  bool dateChanged;
  bool petFrameDue;
  bool metersChanged;
  bool forceMeters;
  bool activitySurfaceChanged;
  bool promptExited;
};

struct SharedClockFaceRenderDecision {
  bool fullRepaint;
  bool clearSurface;
  bool drawTime;
  bool drawDate;
  bool drawPet;
  bool drawMeters;
};

struct SharedClockTimePolicy {
  bool usePlaceholder;
};

static constexpr uint32_t SHARED_CLOCK_PET_FRAME_INTERVAL_MS = 200;

inline constexpr SharedClockFaceLayout sharedClockFaceLayout(bool landscape) {
  return landscape
    ? SharedClockFaceLayout{
        240,
        135,
        {0, 0, 115, 90},
        {
          {129, 46, 60, 16, SHARED_CLOCK_TEXT_PRIMARY},
          {189, 46, 36, 16, SHARED_CLOCK_TEXT_DIM},
          54,
          2,
        },
        {177, 86, 1},
        119,
        16,
      }
    : SharedClockFaceLayout{
        135,
        240,
        {0, 0, 135, 90},
        {
          {19, 166, 60, 16, SHARED_CLOCK_TEXT_PRIMARY},
          {79, 166, 36, 16, SHARED_CLOCK_TEXT_DIM},
          174,
          2,
        },
        {67, 202, 1},
        224,
        16,
      };
}

inline constexpr bool sharedClockFaceSelected(
  const SharedClockFaceContext& context,
  SharedClockActivity activity
) {
  (void)activity;
  return context.normalDisplay &&
    !context.menuVisible &&
    !context.settingsVisible &&
    !context.resetVisible &&
    !context.passkeyVisible &&
    !context.promptVisible &&
    !context.functionalOverrideVisible &&
    !context.otaProgressVisible;
}

inline constexpr bool sharedClockPetFrameDue(uint32_t now, uint32_t nextFrameAt) {
  return (int32_t)(now - nextFrameAt) >= 0;
}

inline constexpr SharedClockFaceRenderDecision sharedClockFaceRenderDecision(
  const SharedClockFaceRenderInput& input
) {
  const bool fullRepaint = input.firstEntry ||
    input.orientationChanged ||
    input.fullRepaintRequested ||
    input.promptExited;
  return {
    fullRepaint,
    fullRepaint,
    fullRepaint || input.secondChanged,
    fullRepaint || input.dateChanged,
    fullRepaint || input.petFrameDue,
    fullRepaint || input.metersChanged || input.forceMeters,
  };
}

inline constexpr SharedClockTimePolicy sharedClockTimePolicy(bool timeValid) {
  return {!timeValid};
}
