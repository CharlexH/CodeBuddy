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
  int16_t statusLabelX;
  int16_t statusY;
  int16_t statusDotX;
  int16_t statusDotY;
  uint8_t statusDotSize;
  uint8_t statusDotRadius;
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
static constexpr uint16_t LANDSCAPE_DASHBOARD_MINT = 0x67FA;
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
static constexpr uint8_t LANDSCAPE_DASHBOARD_HEARTBEAT_SAMPLE_PITCH = 3;
static constexpr uint8_t LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT = 64;
static constexpr uint8_t LANDSCAPE_DASHBOARD_TOKEN_MAX_HEIGHT = 7;

enum LandscapeDashboardHeartbeatSource : uint8_t {
  LANDSCAPE_HEARTBEAT_NONE,
  LANDSCAPE_HEARTBEAT_ACTIVITY,
  LANDSCAPE_HEARTBEAT_TOKEN,
};
inline constexpr LandscapeDashboardLayout landscapeDashboardLayout() {
  return {
    240, 135,
    16, 3, 4, 7, 8, 2,
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

inline constexpr LandscapeDashboardHeartbeatSource
landscapeDashboardHeartbeatSource(bool tokenValid, bool hasActivity20) {
  return tokenValid
    ? LANDSCAPE_HEARTBEAT_TOKEN
    : (hasActivity20 ? LANDSCAPE_HEARTBEAT_ACTIVITY : LANDSCAPE_HEARTBEAT_NONE);
}

inline constexpr int16_t landscapeDashboardTokenHeartbeatY(
  uint8_t intensity,
  int16_t centerY
) {
  return centerY - static_cast<int16_t>(
    (static_cast<uint16_t>(intensity) *
      LANDSCAPE_DASHBOARD_TOKEN_MAX_HEIGHT + 127U) / 255U
  );
}

inline uint8_t landscapeDashboardTokenHeartbeatFrameToken(
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  const uint32_t elapsedMs = nowMs - receivedAtMs;
  return elapsedMs >= 20000U
    ? LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT
    : static_cast<uint8_t>(
      (static_cast<uint64_t>(elapsedMs) *
        LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT) / 20000U
    );
}

inline uint8_t landscapeDashboardTokenHeartbeatCurveIntensity(
  const uint8_t intensities[LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT],
  uint8_t pointIndex
) {
  if (!intensities || pointIndex == 0 ||
      pointIndex == LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT - 1) {
    return intensities ? intensities[pointIndex] : 0;
  }

  const int16_t p0 = intensities[pointIndex > 1 ? pointIndex - 2 : 0];
  const int16_t p1 = intensities[pointIndex - 1];
  const int16_t p2 = intensities[pointIndex];
  const int16_t p3 = intensities[pointIndex + 1];
  const int16_t delta = p2 - p1;
  int16_t slope1 = (p2 - p0) / 2;
  int16_t slope2 = (p3 - p1) / 2;
  if (delta == 0) {
    slope1 = 0;
    slope2 = 0;
  } else {
    if ((slope1 < 0) != (delta < 0)) slope1 = 0;
    if ((slope2 < 0) != (delta < 0)) slope2 = 0;
  }

  // Cubic Hermite at t=0.5. Clamp to the segment endpoints so a sharp
  // change can never overshoot below the centerline or above full scale.
  int16_t midpoint = static_cast<int16_t>(
    (4 * (p1 + p2) + slope1 - slope2 + 4) / 8
  );
  const int16_t low = p1 < p2 ? p1 : p2;
  const int16_t high = p1 > p2 ? p1 : p2;
  if (midpoint < low) midpoint = low;
  if (midpoint > high) midpoint = high;
  return static_cast<uint8_t>(midpoint);
}

inline constexpr uint8_t landscapeDashboardRgb565Red(uint16_t color) {
  return static_cast<uint8_t>((color >> 11) & 0x1f);
}

inline constexpr uint8_t landscapeDashboardRgb565Green(uint16_t color) {
  return static_cast<uint8_t>((color >> 5) & 0x3f);
}

inline constexpr uint8_t landscapeDashboardRgb565Blue(uint16_t color) {
  return static_cast<uint8_t>(color & 0x1f);
}

inline constexpr uint8_t landscapeDashboardBlendChannel(
  uint8_t from,
  uint8_t to,
  uint8_t amount,
  uint16_t denominator
) {
  return static_cast<uint8_t>(
    (static_cast<uint32_t>(from) * (denominator - amount) +
      static_cast<uint32_t>(to) * amount + (denominator / 2U)) /
    denominator
  );
}

inline uint16_t landscapeDashboardTokenHeartbeatColor(
  uint8_t intensity
) {
  if (intensity <= 1) return LANDSCAPE_DASHBOARD_GREEN;
  if (intensity == 255) return LANDSCAPE_DASHBOARD_MINT;
  const uint8_t amount = intensity - 1;
  return static_cast<uint16_t>(
    (static_cast<uint16_t>(landscapeDashboardBlendChannel(
      landscapeDashboardRgb565Red(LANDSCAPE_DASHBOARD_GREEN),
      landscapeDashboardRgb565Red(LANDSCAPE_DASHBOARD_MINT),
      amount,
      254
    )) << 11) |
    (static_cast<uint16_t>(landscapeDashboardBlendChannel(
      landscapeDashboardRgb565Green(LANDSCAPE_DASHBOARD_GREEN),
      landscapeDashboardRgb565Green(LANDSCAPE_DASHBOARD_MINT),
      amount,
      254
    )) << 5) |
    landscapeDashboardBlendChannel(
      landscapeDashboardRgb565Blue(LANDSCAPE_DASHBOARD_GREEN),
      landscapeDashboardRgb565Blue(LANDSCAPE_DASHBOARD_MINT),
      amount,
      254
    )
  );
}

template <typename Canvas>
inline void landscapeDashboardDrawTokenHeartbeatCurve(
  Canvas& canvas,
  const uint8_t intensities[LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT],
  int16_t originX,
  int16_t centerY
) {
  if (!intensities) return;
  uint8_t previousIntensity = landscapeDashboardTokenHeartbeatCurveIntensity(
    intensities, 0
  );
  int16_t previousY = landscapeDashboardTokenHeartbeatY(
    previousIntensity, centerY
  );
  for (uint8_t x = 1; x < LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT; ++x) {
    const uint8_t nextIntensity =
      landscapeDashboardTokenHeartbeatCurveIntensity(intensities, x);
    const int16_t nextY = landscapeDashboardTokenHeartbeatY(
      nextIntensity, centerY
    );
    const uint8_t segmentIntensity = nextIntensity > previousIntensity
      ? nextIntensity
      : previousIntensity;
    if (segmentIntensity == 0) {
      previousIntensity = nextIntensity;
      previousY = nextY;
      continue;
    }
    canvas.drawSmoothLine(
      originX + x - 1,
      previousY,
      originX + x,
      nextY,
      landscapeDashboardTokenHeartbeatColor(segmentIntensity)
    );
    previousIntensity = nextIntensity;
    previousY = nextY;
  }
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

inline uint8_t landscapeDashboardHeartbeatFrameToken(
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  const uint32_t elapsedMs = nowMs - receivedAtMs;
  if (elapsedMs >= 20000U) return 60;
  return static_cast<uint8_t>(
    (elapsedMs * LANDSCAPE_DASHBOARD_HEARTBEAT_SAMPLE_PITCH) / 1000U
  );
}

inline uint8_t landscapeDashboardHeartbeatScrollPixels(
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  return landscapeDashboardHeartbeatFrameToken(receivedAtMs, nowMs) %
    LANDSCAPE_DASHBOARD_HEARTBEAT_SAMPLE_PITCH;
}

inline constexpr uint32_t landscapeDashboardHeartbeatLatestBucket(
  uint32_t receivedAtMs,
  uint32_t nowMs
) {
  return (receivedAtMs / 1000U) + ((nowMs - receivedAtMs) / 1000U);
}

inline constexpr int8_t landscapeDashboardHeartbeatOffset(uint8_t) {
  return -6;
}

inline int16_t landscapeDashboardHeartbeatCurveY(
  uint32_t activityMask,
  uint32_t latestBucket,
  uint8_t pointIndex,
  int16_t centerY
) {
  if (pointIndex == 0 || pointIndex > 20) return centerY;
  const uint8_t activityIndex = pointIndex - 1;
  if (!landscapeDashboardActivityVisibleAt(activityMask, activityIndex)) {
    return centerY;
  }
  const uint8_t age = 19 - activityIndex;
  return centerY + landscapeDashboardHeartbeatOffset(latestBucket - age);
}

inline int16_t landscapeDashboardRoundCurveCoordinate(float value) {
  return static_cast<int16_t>(value + (value < 0.0f ? -0.5f : 0.5f));
}

template <typename Canvas>
inline void landscapeDashboardDrawHeartbeatCurve(
  Canvas& canvas,
  uint32_t activityMask,
  uint32_t latestBucket,
  int16_t originX,
  int16_t centerY,
  uint16_t color,
  uint8_t scrollPixels
) {
  static constexpr uint8_t pointCount = 22;
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
      activityMask, latestBucket, p0Index, centerY
    );
    const float p1 = landscapeDashboardHeartbeatCurveY(
      activityMask, latestBucket, p1Index, centerY
    );
    const float p2 = landscapeDashboardHeartbeatCurveY(
      activityMask, latestBucket, p2Index, centerY
    );
    const float p3 = landscapeDashboardHeartbeatCurveY(
      activityMask, latestBucket, p3Index, centerY
    );
    int16_t previousX = originX +
      (segment * LANDSCAPE_DASHBOARD_HEARTBEAT_SAMPLE_PITCH) - scrollPixels;
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
