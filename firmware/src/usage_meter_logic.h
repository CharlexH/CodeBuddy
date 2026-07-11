#pragma once

#include <stdint.h>

struct UsageMeterState {
  bool hasUsageLimits;
  uint8_t fiveHourRemaining;
  uint8_t sevenDayRemaining;
};

static constexpr uint8_t USAGE_METER_HEIGHT = 6;
static constexpr uint8_t USAGE_METER_LANE_HEIGHT = 3;
static constexpr uint16_t USAGE_METER_CONSUMED = 0x10A2;
static constexpr uint16_t USAGE_METER_FIVE_HOUR = 0x07E0;
static constexpr uint16_t USAGE_METER_SEVEN_DAY = 0x03A0;

struct UsageMeterRect {
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint8_t height;
  uint16_t color;
};

struct UsageMeterRenderPlan {
  uint8_t count;
  UsageMeterRect rects[4];
};

inline bool usageMeterValidPair(int fiveHourRemaining, int sevenDayRemaining) {
  return fiveHourRemaining >= 0 && fiveHourRemaining <= 100 &&
         sevenDayRemaining >= 0 && sevenDayRemaining <= 100;
}

inline void usageMeterClear(UsageMeterState* state) {
  if (state == nullptr) return;
  state->hasUsageLimits = false;
  state->fiveHourRemaining = 0;
  state->sevenDayRemaining = 0;
}

inline void usageMeterApply(
  UsageMeterState* state,
  bool usageObjectPresent,
  bool usageObjectHasIntegerPair,
  int fiveHourRemaining,
  int sevenDayRemaining
) {
  if (state == nullptr || !usageObjectPresent) return;

  if (!usageObjectHasIntegerPair || !usageMeterValidPair(fiveHourRemaining, sevenDayRemaining)) {
    usageMeterClear(state);
    return;
  }

  state->hasUsageLimits = true;
  state->fiveHourRemaining = (uint8_t)fiveHourRemaining;
  state->sevenDayRemaining = (uint8_t)sevenDayRemaining;
}

inline uint16_t usageMeterFillWidth(uint16_t fullWidth, int remainingPercent) {
  if (remainingPercent <= 0) return 0;
  if (remainingPercent >= 100) return fullWidth;
  return (uint16_t)(((uint32_t)fullWidth * (uint32_t)remainingPercent) / 100U);
}

inline uint8_t usageMeterFooterInset(bool connected, const UsageMeterState& state) {
  return connected && state.hasUsageLimits &&
      usageMeterValidPair(state.fiveHourRemaining, state.sevenDayRemaining)
    ? USAGE_METER_HEIGHT
    : 0;
}

inline UsageMeterRenderPlan usageMeterRenderPlan(
  const UsageMeterState& state,
  uint16_t fullWidth,
  uint16_t fullHeight
) {
  UsageMeterRenderPlan plan = {};
  if (!state.hasUsageLimits ||
      !usageMeterValidPair(state.fiveHourRemaining, state.sevenDayRemaining) ||
      fullWidth == 0 || fullHeight < USAGE_METER_HEIGHT) {
    return plan;
  }

  uint16_t topY = fullHeight - USAGE_METER_HEIGHT;
  uint16_t bottomY = topY + USAGE_METER_LANE_HEIGHT;
  plan.count = 4;
  plan.rects[0] = {0, topY, fullWidth, USAGE_METER_LANE_HEIGHT, USAGE_METER_CONSUMED};
  plan.rects[1] = {
    0,
    topY,
    usageMeterFillWidth(fullWidth, state.fiveHourRemaining),
    USAGE_METER_LANE_HEIGHT,
    USAGE_METER_FIVE_HOUR,
  };
  plan.rects[2] = {0, bottomY, fullWidth, USAGE_METER_LANE_HEIGHT, USAGE_METER_CONSUMED};
  plan.rects[3] = {
    0,
    bottomY,
    usageMeterFillWidth(fullWidth, state.sevenDayRemaining),
    USAGE_METER_LANE_HEIGHT,
    USAGE_METER_SEVEN_DAY,
  };
  return plan;
}
