#pragma once

#include <stdint.h>
#include <stdio.h>

enum StatusDashboardKind : uint8_t {
  STATUS_RUN,
  STATUS_ASK,
  STATUS_NEW,
};

enum StatusDashboardColorRole : uint8_t {
  STATUS_COLOR_DIM,
  STATUS_COLOR_GREEN,
  STATUS_COLOR_AMBER,
  STATUS_COLOR_CYAN,
};

struct StatusDashboardCounts {
  bool hasUnread;
  uint8_t unread;
};

static constexpr uint8_t STATUS_DASHBOARD_ASCII_FONT_X_ADVANCE = 10;
static constexpr uint8_t STATUS_DASHBOARD_ASCII_FONT_Y_ADVANCE = 21;
static constexpr float STATUS_DASHBOARD_LABEL_TEXT_SIZE = 0.57f;
static constexpr float STATUS_DASHBOARD_COUNT_TEXT_SIZE = 0.76f;
static constexpr int16_t STATUS_DASHBOARD_LABEL_CENTER_Y = 11;
static constexpr int16_t STATUS_DASHBOARD_COUNT_CENTER_Y = 40;

inline constexpr float statusDashboardAsciiTextWidth(
  uint8_t characterCount,
  float textSize
) {
  return characterCount * STATUS_DASHBOARD_ASCII_FONT_X_ADVANCE * textSize;
}

inline constexpr float statusDashboardAsciiTextHeight(float textSize) {
  return STATUS_DASHBOARD_ASCII_FONT_Y_ADVANCE * textSize;
}

inline constexpr int16_t statusDashboardLabelCenterY(int16_t originY) {
  return originY + STATUS_DASHBOARD_LABEL_CENTER_Y;
}

inline constexpr int16_t statusDashboardCountCenterY(int16_t originY) {
  return originY + STATUS_DASHBOARD_COUNT_CENTER_Y;
}

inline void statusDashboardApplyUnread(
  StatusDashboardCounts* counts,
  bool fieldPresent,
  bool fieldValid,
  int unread
) {
  if (counts == nullptr || !fieldPresent || !fieldValid || unread < 0 || unread > 255) {
    return;
  }
  counts->hasUnread = true;
  counts->unread = static_cast<uint8_t>(unread);
}

inline void statusDashboardFormatCount(char* output, size_t size, uint8_t count) {
  if (output == nullptr || size == 0) return;
  if (count > 99) snprintf(output, size, "99+");
  else snprintf(output, size, "%u", static_cast<unsigned>(count));
}

inline StatusDashboardColorRole statusDashboardColorRole(
  StatusDashboardKind kind,
  uint8_t count
) {
  if (count == 0) return STATUS_COLOR_DIM;
  switch (kind) {
    case STATUS_ASK: return STATUS_COLOR_AMBER;
    case STATUS_NEW: return STATUS_COLOR_CYAN;
    case STATUS_RUN:
    default: return STATUS_COLOR_GREEN;
  }
}
