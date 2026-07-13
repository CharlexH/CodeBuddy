#pragma once

#include <stdint.h>

struct UsageMeterState {
  bool hasFiveHour;
  bool hasSevenDay;
  uint8_t fiveHourRemaining;
  uint8_t sevenDayRemaining;
};

static constexpr uint8_t USAGE_METER_BAR_HEIGHT = 6;
static constexpr uint8_t USAGE_METER_GAP = 2;
static constexpr uint8_t USAGE_METER_SIDE_INSET = 2;
static constexpr uint8_t USAGE_METER_BOTTOM_INSET = 2;
static constexpr uint8_t USAGE_METER_SINGLE_FOOTPRINT =
  USAGE_METER_BAR_HEIGHT + USAGE_METER_BOTTOM_INSET;
static constexpr uint8_t USAGE_METER_FOOTPRINT =
  (USAGE_METER_BAR_HEIGHT * 2) + USAGE_METER_GAP + USAGE_METER_BOTTOM_INSET;
static constexpr uint8_t USAGE_METER_MIN_WIDTH = (USAGE_METER_SIDE_INSET * 2) + 1;
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

struct UsageMeterRenderState {
  bool visible;
  bool hasFiveHour;
  bool hasSevenDay;
  uint8_t fiveHourRemaining;
  uint8_t sevenDayRemaining;
};

struct UsageMeterRenderDecision {
  bool draw;
  bool clear;
};

struct UsageMeterRenderFrame {
  UsageMeterRenderPlan plan;
  UsageMeterRenderDecision decision;
};

inline bool usageMeterValidPercent(int remaining) {
  return remaining >= 0 && remaining <= 100;
}

inline bool usageMeterStateValid(const UsageMeterState& state) {
  if (!state.hasFiveHour && !state.hasSevenDay) return false;
  return (!state.hasFiveHour || usageMeterValidPercent(state.fiveHourRemaining)) &&
         (!state.hasSevenDay || usageMeterValidPercent(state.sevenDayRemaining));
}

inline void usageMeterClear(UsageMeterState* state) {
  if (state == nullptr) return;
  state->hasFiveHour = false;
  state->hasSevenDay = false;
  state->fiveHourRemaining = 0;
  state->sevenDayRemaining = 0;
}

inline void usageMeterApply(
  UsageMeterState* state,
  bool usageObjectPresent,
  bool usageObjectValid,
  bool hasFiveHour,
  int fiveHourRemaining,
  bool hasSevenDay,
  int sevenDayRemaining
) {
  if (state == nullptr || !usageObjectPresent) return;

  if (!usageObjectValid || (!hasFiveHour && !hasSevenDay) ||
      (hasFiveHour && !usageMeterValidPercent(fiveHourRemaining)) ||
      (hasSevenDay && !usageMeterValidPercent(sevenDayRemaining))) {
    usageMeterClear(state);
    return;
  }

  state->hasFiveHour = hasFiveHour;
  state->hasSevenDay = hasSevenDay;
  state->fiveHourRemaining = hasFiveHour ? (uint8_t)fiveHourRemaining : 0;
  state->sevenDayRemaining = hasSevenDay ? (uint8_t)sevenDayRemaining : 0;
}

inline uint16_t usageMeterFillWidth(uint16_t fullWidth, int remainingPercent) {
  if (remainingPercent <= 0) return 0;
  if (remainingPercent >= 100) return fullWidth;
  return (uint16_t)(((uint32_t)fullWidth * (uint32_t)remainingPercent) / 100U);
}

inline uint8_t usageMeterFooterInset(bool connected, const UsageMeterState& state) {
  if (!connected || !usageMeterStateValid(state)) return 0;
  return state.hasFiveHour && state.hasSevenDay
    ? USAGE_METER_FOOTPRINT
    : USAGE_METER_SINGLE_FOOTPRINT;
}

inline UsageMeterRenderDecision usageMeterRenderTransition(
  UsageMeterRenderState* state,
  bool visible,
  bool hasFiveHour = false,
  bool hasSevenDay = false,
  uint8_t fiveHourRemaining = 0,
  uint8_t sevenDayRemaining = 0,
  bool forceDraw = false
) {
  if (state == nullptr) return {visible, false};
  const bool wasVisible = state->visible;
  if (!visible) {
    state->visible = false;
    state->hasFiveHour = false;
    state->hasSevenDay = false;
    state->fiveHourRemaining = 0;
    state->sevenDayRemaining = 0;
    return {false, wasVisible};
  }

  const bool layoutChanged = wasVisible &&
    (state->hasFiveHour != hasFiveHour || state->hasSevenDay != hasSevenDay);
  const bool valuesChanged = !wasVisible || layoutChanged ||
    state->fiveHourRemaining != fiveHourRemaining ||
    state->sevenDayRemaining != sevenDayRemaining;
  state->visible = true;
  state->hasFiveHour = hasFiveHour;
  state->hasSevenDay = hasSevenDay;
  state->fiveHourRemaining = fiveHourRemaining;
  state->sevenDayRemaining = sevenDayRemaining;
  return {forceDraw || valuesChanged, layoutChanged};
}

inline void usageMeterRenderReset(UsageMeterRenderState* state) {
  if (state == nullptr) return;
  state->visible = false;
  state->hasFiveHour = false;
  state->hasSevenDay = false;
  state->fiveHourRemaining = 0;
  state->sevenDayRemaining = 0;
}

inline UsageMeterRenderPlan usageMeterRenderPlan(
  const UsageMeterState& state,
  uint16_t fullWidth,
  uint16_t fullHeight
) {
  UsageMeterRenderPlan plan = {};
  const uint8_t footprint = state.hasFiveHour && state.hasSevenDay
    ? USAGE_METER_FOOTPRINT
    : USAGE_METER_SINGLE_FOOTPRINT;
  if (!usageMeterStateValid(state) || fullWidth < USAGE_METER_MIN_WIDTH ||
      fullHeight < footprint) {
    return plan;
  }

  const uint16_t usableWidth = fullWidth - (USAGE_METER_SIDE_INSET * 2);
  const uint16_t firstY = fullHeight - footprint;
  if (state.hasFiveHour && state.hasSevenDay) {
    const uint16_t secondY = firstY + USAGE_METER_BAR_HEIGHT + USAGE_METER_GAP;
    plan.count = 4;
    plan.rects[0] = {USAGE_METER_SIDE_INSET, firstY, usableWidth,
                     USAGE_METER_BAR_HEIGHT, USAGE_METER_CONSUMED};
    plan.rects[1] = {USAGE_METER_SIDE_INSET, firstY,
                     usageMeterFillWidth(usableWidth, state.fiveHourRemaining),
                     USAGE_METER_BAR_HEIGHT, USAGE_METER_FIVE_HOUR};
    plan.rects[2] = {USAGE_METER_SIDE_INSET, secondY, usableWidth,
                     USAGE_METER_BAR_HEIGHT, USAGE_METER_CONSUMED};
    plan.rects[3] = {USAGE_METER_SIDE_INSET, secondY,
                     usageMeterFillWidth(usableWidth, state.sevenDayRemaining),
                     USAGE_METER_BAR_HEIGHT, USAGE_METER_SEVEN_DAY};
    return plan;
  }

  const int remaining = state.hasFiveHour
    ? state.fiveHourRemaining
    : state.sevenDayRemaining;
  const uint16_t color = state.hasFiveHour
    ? USAGE_METER_FIVE_HOUR
    : USAGE_METER_SEVEN_DAY;
  plan.count = 2;
  plan.rects[0] = {USAGE_METER_SIDE_INSET, firstY, usableWidth,
                   USAGE_METER_BAR_HEIGHT, USAGE_METER_CONSUMED};
  plan.rects[1] = {USAGE_METER_SIDE_INSET, firstY,
                   usageMeterFillWidth(usableWidth, remaining),
                   USAGE_METER_BAR_HEIGHT, color};
  return plan;
}

inline UsageMeterRenderFrame usageMeterPrepareFrame(
  UsageMeterRenderState* renderState,
  bool connected,
  const UsageMeterState& usage,
  uint16_t fullWidth,
  uint16_t fullHeight,
  bool forceDraw = false
) {
  UsageMeterRenderPlan plan = connected
    ? usageMeterRenderPlan(usage, fullWidth, fullHeight)
    : UsageMeterRenderPlan{};
  return {
    plan,
    usageMeterRenderTransition(
      renderState,
      plan.count > 0,
      usage.hasFiveHour,
      usage.hasSevenDay,
      usage.fiveHourRemaining,
      usage.sevenDayRemaining,
      forceDraw
    ),
  };
}
