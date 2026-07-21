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
static constexpr uint16_t LANDSCAPE_DASHBOARD_RUN_TINT = 0x1142;
static constexpr uint16_t LANDSCAPE_DASHBOARD_ASK_TINT = 0x2105;
static constexpr uint16_t LANDSCAPE_DASHBOARD_NEW_TINT = 0x1925;
static constexpr uint32_t LANDSCAPE_DASHBOARD_ACTIVITY_MASK = 0x000FFFFFUL;
inline constexpr LandscapeDashboardLayout landscapeDashboardLayout() {
  return {
    240, 135,
    4, 4, 5,
    172, 4, 64, 14, 11,
    4, 34,
    129, 44,
    129, 36,
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

inline constexpr uint8_t landscapeDashboardHeartbeatHeight(uint8_t index) {
  return (index % 8) == 0 ? 4
    : (index % 8) == 1 ? 8
    : (index % 8) == 2 ? 12
    : (index % 8) == 3 ? 7
    : (index % 8) == 4 ? 9
    : (index % 8) == 5 ? 12
    : (index % 8) == 6 ? 5
    : 10;
}
