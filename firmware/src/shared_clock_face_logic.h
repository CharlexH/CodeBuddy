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

enum SharedClockDateMode : uint8_t {
  SHARED_CLOCK_DATE_SINGLE_LINE,
  SHARED_CLOCK_DATE_STACKED_NUMERIC,
};

struct SharedClockPetLayout {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  uint8_t asciiScale;
  int16_t asciiYOffset;
  bool useLocalSurface;
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
  bool showSeconds;
};

struct SharedClockDateLayout {
  SharedClockDateMode mode;
  int16_t centerX;
  int16_t centerY;
  SharedClockTextRect month;
  SharedClockTextRect day;
  uint8_t textSize;
};

struct SharedClockStatusLayout {
  bool visible;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  uint8_t columnWidth;
};

struct SharedClockFaceLayout {
  uint16_t screenWidth;
  uint16_t screenHeight;
  SharedClockPetLayout pet;
  SharedClockTimeLayout time;
  SharedClockDateLayout date;
  SharedClockStatusLayout status;
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
  bool statusVisible;
  bool statusChanged;
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
  bool drawStatus;
  bool drawMeters;
};

struct SharedClockStatusCounts {
  uint8_t running;
  uint8_t waiting;
  uint8_t unread;
};

struct SharedClockTimePolicy {
  bool usePlaceholder;
};

static constexpr uint32_t SHARED_CLOCK_PET_FRAME_INTERVAL_MS = 200;

inline constexpr bool sharedClockPetLocalSurfaceNeedsClear(
  bool asciiMode,
  bool fullRepaint
) {
  return asciiMode || fullRepaint;
}

inline constexpr SharedClockFaceLayout sharedClockFaceLayout(bool landscape) {
  return landscape
    ? SharedClockFaceLayout{
        240,
        135,
        {0, 0, 120, 58, 1, -13, true},
        {
          {8, 77, 120, 32, SHARED_CLOCK_TEXT_PRIMARY},
          {128, 77, 72, 32, SHARED_CLOCK_TEXT_DIM},
          93,
          4,
          true,
        },
        {
          SHARED_CLOCK_DATE_STACKED_NUMERIC,
          0,
          0,
          {208, 74, 24, 16, SHARED_CLOCK_TEXT_DIM},
          {208, 94, 24, 16, SHARED_CLOCK_TEXT_DIM},
          2,
        },
        {true, 120, 0, 120, 58, 40},
        111,
        24,
      }
    : SharedClockFaceLayout{
        135,
        240,
        {0, 0, 135, 90, 1, 0, false},
        {
          {19, 166, 60, 16, SHARED_CLOCK_TEXT_PRIMARY},
          {79, 166, 36, 16, SHARED_CLOCK_TEXT_DIM},
          174,
          2,
          true,
        },
        {
          SHARED_CLOCK_DATE_SINGLE_LINE,
          67,
          202,
          {0, 0, 0, 0, SHARED_CLOCK_TEXT_DIM},
          {0, 0, 0, 0, SHARED_CLOCK_TEXT_DIM},
          1,
        },
        {false, 0, 0, 0, 0, 0},
        224,
        16,
      };
}

inline constexpr bool sharedClockFaceSelected(
  const SharedClockFaceContext& context,
  SharedClockActivity activity
) {
  return (activity == SHARED_CLOCK_IDLE ||
      activity == SHARED_CLOCK_ACTIVE ||
      activity == SHARED_CLOCK_WAITING) &&
    context.normalDisplay &&
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

inline constexpr bool sharedClockFaceFullRepaint(
  const SharedClockFaceRenderInput& input
) {
  return input.firstEntry ||
    input.orientationChanged ||
    input.fullRepaintRequested ||
    input.promptExited;
}

inline constexpr SharedClockFaceRenderDecision sharedClockFaceRenderDecision(
  const SharedClockFaceRenderInput& input
) {
  return {
    sharedClockFaceFullRepaint(input),
    sharedClockFaceFullRepaint(input),
    sharedClockFaceFullRepaint(input) || input.secondChanged,
    sharedClockFaceFullRepaint(input) || input.dateChanged,
    sharedClockFaceFullRepaint(input) || input.petFrameDue,
    input.statusVisible &&
      (sharedClockFaceFullRepaint(input) || input.statusChanged),
    sharedClockFaceFullRepaint(input) || input.metersChanged || input.forceMeters,
  };
}

inline constexpr SharedClockTimePolicy sharedClockTimePolicy(bool timeValid) {
  return {!timeValid};
}
