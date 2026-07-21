#pragma once

#include <stdint.h>

enum LandscapeDashboardStatus : uint8_t {
  DASHBOARD_RUNNING,
  DASHBOARD_WAITING,
  DASHBOARD_IDLE,
  DASHBOARD_OFFLINE,
};

struct LandscapeDashboardLayout {
  uint16_t screenWidth;
  uint16_t screenHeight;
  int16_t statusX;
  int16_t statusY;
  int16_t statusDotY;
  int16_t heartbeatX;
  int16_t heartbeatY;
  uint8_t heartbeatWidth;
  uint8_t heartbeatHeight;
  int16_t heartbeatCenterY;
  int16_t timeX;
  int16_t timeY;
  int16_t secondsX;
  int16_t secondsY;
  int16_t secondProgressX;
  int16_t secondProgressY;
  int16_t dateY;
  int16_t cardsX;
  uint8_t cardsWidth;
  uint8_t cardHeight;
  int16_t cardsY[3];
  uint16_t meterY;
  uint8_t meterFootprint;
};

static constexpr uint16_t LANDSCAPE_DASHBOARD_BG = 0x18C3;
static constexpr uint16_t LANDSCAPE_DASHBOARD_GREEN = 0x07A0;
static constexpr uint16_t LANDSCAPE_DASHBOARD_RUN = 0x07E0;
static constexpr uint16_t LANDSCAPE_DASHBOARD_WAITING = 0xFF80;
static constexpr uint16_t LANDSCAPE_DASHBOARD_IDLE = 0xFFFF;
static constexpr uint16_t LANDSCAPE_DASHBOARD_OFFLINE = 0xFA8A;
static constexpr uint16_t LANDSCAPE_DASHBOARD_ASK = 0xCCFF;
static constexpr uint16_t LANDSCAPE_DASHBOARD_NEW = 0x5E1F;
static constexpr uint16_t LANDSCAPE_DASHBOARD_DIM = 0x738E;
static constexpr uint16_t LANDSCAPE_DASHBOARD_WHITE_40 = 0x738E;
static constexpr uint16_t LANDSCAPE_DASHBOARD_SECONDS_TEXT = 0xA514;
static constexpr uint16_t LANDSCAPE_DASHBOARD_DATE_TEXT = 0xFFFF;
static constexpr uint16_t LANDSCAPE_DASHBOARD_RUN_TINT = 0x1142;
static constexpr uint16_t LANDSCAPE_DASHBOARD_ASK_TINT = 0x2105;
static constexpr uint16_t LANDSCAPE_DASHBOARD_NEW_TINT = 0x1925;
static constexpr uint32_t LANDSCAPE_DASHBOARD_ACTIVITY_MASK = 0x000FFFFFUL;
static constexpr uint16_t LANDSCAPE_DASHBOARD_HEARTBEAT_FRAME_MS = 50;
inline constexpr LandscapeDashboardLayout landscapeDashboardLayout() {
  return {
    240, 135,
    4, 2, 5,
    172, 4, 64, 14, 11,
    4, 34,
    129, 46,
    129, 34,
    79,
    172, 64, 14, {34, 57, 79},
    109, 26,
  };
}

inline constexpr LandscapeDashboardStatus landscapeDashboardStatus(
  bool connected,
  uint8_t running,
  uint8_t waiting
) {
  return !connected
    ? DASHBOARD_OFFLINE
    : (waiting > 0
      ? DASHBOARD_WAITING
      : (running > 0 ? DASHBOARD_RUNNING : DASHBOARD_IDLE));
}

inline const char* landscapeDashboardStatusLabel(LandscapeDashboardStatus status) {
  switch (status) {
    case DASHBOARD_RUNNING: return "RUNNING";
    case DASHBOARD_WAITING: return "WAITING";
    case DASHBOARD_IDLE: return "IDLE";
    case DASHBOARD_OFFLINE:
    default: return "OFFLINE";
  }
}

inline constexpr uint16_t landscapeDashboardStatusColor(
  LandscapeDashboardStatus status
) {
  return status == DASHBOARD_RUNNING
    ? LANDSCAPE_DASHBOARD_GREEN
    : (status == DASHBOARD_WAITING
      ? LANDSCAPE_DASHBOARD_WAITING
      : (status == DASHBOARD_IDLE
        ? LANDSCAPE_DASHBOARD_IDLE
        : LANDSCAPE_DASHBOARD_OFFLINE));
}

inline constexpr uint16_t landscapeDashboardCountColor(
  uint8_t count,
  uint16_t activeColor
) {
  return count == 0 ? LANDSCAPE_DASHBOARD_WHITE_40 : activeColor;
}

inline constexpr uint8_t landscapeDashboardSecondBlocks(int seconds) {
  return seconds <= 0 || seconds > 59
    ? 0
    : static_cast<uint8_t>((seconds + 14) / 15);
}

inline constexpr uint32_t landscapeDashboardActivityMaskAt(
  uint32_t receivedMask,
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  return ((nowMs - receivedAtMs) / 1000U) >= 20U
    ? 0U
    : ((receivedMask << ((nowMs - receivedAtMs) / 1000U)) &
      LANDSCAPE_DASHBOARD_ACTIVITY_MASK);
}

inline constexpr bool landscapeDashboardActivityVisibleAt(
  uint32_t activityMask,
  uint8_t leftToRightIndex
) {
  return leftToRightIndex < 20 &&
    (activityMask & (1UL << (19 - leftToRightIndex))) != 0;
}

inline constexpr uint16_t landscapeDashboardHeartbeatPhasePermille(
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  return static_cast<uint16_t>((nowMs - receivedAtMs) % 1000U);
}

inline constexpr bool landscapeDashboardHeartbeatFrameDue(
  uint32_t nowMs,
  uint32_t lastFrameAtMs,
  bool activityChanged,
  bool force
) {
  return force || activityChanged || lastFrameAtMs == UINT32_MAX ||
    (nowMs - lastFrameAtMs) >= LANDSCAPE_DASHBOARD_HEARTBEAT_FRAME_MS;
}

inline constexpr int8_t landscapeDashboardHeartbeatOffset(uint8_t index) {
  return (index % 8) == 0 ? -2
    : (index % 8) == 1 ? 3
    : (index % 8) == 2 ? -5
    : (index % 8) == 3 ? 4
    : (index % 8) == 4 ? -3
    : (index % 8) == 5 ? 5
    : (index % 8) == 6 ? -4
    : 2;
}

inline int16_t landscapeDashboardHeartbeatCurveY(
  uint32_t activityMask,
  uint8_t pointIndex,
  int16_t centerY,
  uint16_t phasePermille
) {
  const int16_t currentOffset = pointIndex > 0 && pointIndex <= 20 &&
      landscapeDashboardActivityVisibleAt(activityMask, pointIndex - 1)
    ? landscapeDashboardHeartbeatOffset(pointIndex - 1)
    : 0;
  const uint8_t nextPointIndex = pointIndex + 1;
  const int16_t nextOffset = nextPointIndex > 0 && nextPointIndex <= 20 &&
      landscapeDashboardActivityVisibleAt(activityMask, nextPointIndex - 1)
    ? landscapeDashboardHeartbeatOffset(nextPointIndex - 1)
    : 0;
  const int32_t interpolated =
    (currentOffset * static_cast<int32_t>(1000U - phasePermille)) +
    (nextOffset * static_cast<int32_t>(phasePermille));
  return centerY + static_cast<int16_t>(
    (interpolated + (interpolated < 0 ? -500 : 500)) / 1000
  );
}

inline int16_t landscapeDashboardRoundCurveCoordinate(float value) {
  return static_cast<int16_t>(value + (value < 0.0f ? -0.5f : 0.5f));
}

template <typename Canvas>
inline void landscapeDashboardDrawHeartbeatCurve(
  Canvas& canvas,
  uint32_t activityMask,
  int16_t originX,
  int16_t centerY,
  uint16_t color,
  uint16_t phasePermille
) {
  static constexpr uint8_t pointCount = 22;
  static constexpr uint8_t pointPitch = 3;
  static constexpr uint8_t curveSubsteps = 3;

  if (activityMask == 0) return;

  for (uint8_t segment = 0; segment < pointCount - 1; ++segment) {

    const uint8_t p0Index = segment == 0 ? 0 : segment - 1;
    const uint8_t p1Index = segment;
    const uint8_t p2Index = segment + 1;
    const uint8_t p3Index = segment + 2 < pointCount
      ? segment + 2
      : pointCount - 1;
    const float p0 = landscapeDashboardHeartbeatCurveY(
      activityMask, p0Index, centerY, phasePermille
    );
    const float p1 = landscapeDashboardHeartbeatCurveY(
      activityMask, p1Index, centerY, phasePermille
    );
    const float p2 = landscapeDashboardHeartbeatCurveY(
      activityMask, p2Index, centerY, phasePermille
    );
    const float p3 = landscapeDashboardHeartbeatCurveY(
      activityMask, p3Index, centerY, phasePermille
    );
    int16_t previousX = originX + (segment * pointPitch);
    int16_t previousY = static_cast<int16_t>(p1);

    for (uint8_t step = 1; step <= curveSubsteps; ++step) {
      const float t = static_cast<float>(step) / curveSubsteps;
      const float t2 = t * t;
      const float t3 = t2 * t;
      const float curveY = 0.5f * (
        (2.0f * p1) +
        ((-p0 + p2) * t) +
        (((2.0f * p0) - (5.0f * p1) + (4.0f * p2) - p3) * t2) +
        ((-p0 + (3.0f * p1) - (3.0f * p2) + p3) * t3)
      );
      const int16_t nextX = previousX + 1;
      const int16_t nextY = landscapeDashboardRoundCurveCoordinate(curveY);
      canvas.drawSmoothLine(previousX, previousY, nextX, nextY, color);
      previousX = nextX;
      previousY = nextY;
    }
  }
}
