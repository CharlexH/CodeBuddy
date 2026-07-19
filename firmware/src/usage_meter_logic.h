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
static constexpr uint16_t LANDSCAPE_USAGE_METER_ACTIVE = 0x07E0;
static constexpr uint16_t LANDSCAPE_USAGE_METER_CONSUMED = 0x19C5;
static constexpr uint16_t LANDSCAPE_USAGE_METER_BLUE_GREEN = 0x05F5;
static constexpr uint8_t LANDSCAPE_USAGE_METER_FOOTPRINT = 20;
static constexpr uint8_t LANDSCAPE_USAGE_METER_TOP_INSET = 1;
static constexpr uint8_t LANDSCAPE_USAGE_METER_DOT_SIZE = 2;
static constexpr uint8_t LANDSCAPE_USAGE_METER_DOT_GAP = 2;
static constexpr uint8_t LANDSCAPE_USAGE_METER_DOT_ROWS = 5;
static constexpr uint8_t LANDSCAPE_USAGE_METER_SCROLL_BAND = 4;
static constexpr uint8_t LANDSCAPE_USAGE_METER_ANIMATION_STEPS = 16;
static constexpr uint8_t LANDSCAPE_USAGE_METER_ANIMATION_FRAME_MS = 50;
static constexpr uint8_t LANDSCAPE_USAGE_METER_GRID_HEIGHT =
  (LANDSCAPE_USAGE_METER_DOT_ROWS * LANDSCAPE_USAGE_METER_DOT_SIZE) +
  ((LANDSCAPE_USAGE_METER_DOT_ROWS - 1) * LANDSCAPE_USAGE_METER_DOT_GAP);

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
  bool dotted;
  uint8_t dotSize;
  uint8_t dotGap;
  uint8_t dotRows;
  uint16_t dotColumns;
  uint16_t dotFilledColumns;
};

struct UsageMeterRenderState {
  bool visible;
  bool hasFiveHour;
  bool hasSevenDay;
  uint8_t fiveHourRemaining;
  uint8_t sevenDayRemaining;
  bool animationActive;
  uint8_t animationFrame;
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
  bool forceDraw = false,
  bool animationActive = false,
  uint8_t animationFrame = 0
) {
  if (state == nullptr) return {visible, false};
  const bool wasVisible = state->visible;
  if (!visible) {
    state->visible = false;
    state->hasFiveHour = false;
    state->hasSevenDay = false;
    state->fiveHourRemaining = 0;
    state->sevenDayRemaining = 0;
    state->animationActive = false;
    state->animationFrame = 0;
    return {false, wasVisible};
  }

  const bool layoutChanged = wasVisible &&
    (state->hasFiveHour != hasFiveHour || state->hasSevenDay != hasSevenDay);
  const bool valuesChanged = !wasVisible || layoutChanged ||
    state->fiveHourRemaining != fiveHourRemaining ||
    state->sevenDayRemaining != sevenDayRemaining ||
    state->animationActive != animationActive ||
    (animationActive && state->animationFrame != animationFrame);
  state->visible = true;
  state->hasFiveHour = hasFiveHour;
  state->hasSevenDay = hasSevenDay;
  state->fiveHourRemaining = fiveHourRemaining;
  state->sevenDayRemaining = sevenDayRemaining;
  state->animationActive = animationActive;
  state->animationFrame = animationActive ? animationFrame : 0;
  return {forceDraw || valuesChanged, layoutChanged};
}

inline void usageMeterRenderReset(UsageMeterRenderState* state) {
  if (state == nullptr) return;
  state->visible = false;
  state->hasFiveHour = false;
  state->hasSevenDay = false;
  state->fiveHourRemaining = 0;
  state->sevenDayRemaining = 0;
  state->animationActive = false;
  state->animationFrame = 0;
}

inline uint8_t usageMeterLandscapeAnimationFrame(uint32_t nowMs) {
  return static_cast<uint8_t>(
    (nowMs / LANDSCAPE_USAGE_METER_ANIMATION_FRAME_MS) %
    LANDSCAPE_USAGE_METER_ANIMATION_STEPS
  );
}

inline bool usageMeterLandscapeAnimationEnabled(
  const UsageMeterRenderPlan& plan,
  uint8_t runningTasks
) {
  return plan.dotted && plan.dotFilledColumns > 0 && runningTasks > 0;
}

inline uint16_t usageMeterRgb565Interpolate(
  uint16_t from,
  uint16_t to,
  uint16_t position,
  uint16_t maximum
) {
  if (maximum == 0 || position == 0) return from;
  if (position >= maximum) return to;
  const uint16_t fromR = (from >> 11) & 0x1F;
  const uint16_t fromG = (from >> 5) & 0x3F;
  const uint16_t fromB = from & 0x1F;
  const uint16_t toR = (to >> 11) & 0x1F;
  const uint16_t toG = (to >> 5) & 0x3F;
  const uint16_t toB = to & 0x1F;
  const uint16_t r = (fromR * (maximum - position) + toR * position) / maximum;
  const uint16_t g = (fromG * (maximum - position) + toG * position) / maximum;
  const uint16_t b = (fromB * (maximum - position) + toB * position) / maximum;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

inline uint16_t usageMeterRgb565Scale(uint16_t color, uint8_t percent) {
  const uint16_t r = (((color >> 11) & 0x1F) * percent) / 100;
  const uint16_t g = (((color >> 5) & 0x3F) * percent) / 100;
  const uint16_t b = ((color & 0x1F) * percent) / 100;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

inline uint8_t usageMeterDiagonalBrightness(
  uint8_t row,
  uint16_t column,
  uint8_t animationFrame
) {
  // Tile the diagonal front every four cells so one complete wave block exits
  // as the next enters, matching gradient-spin without a wraparound jump.
  const uint8_t phaseStep = static_cast<uint8_t>(
    ((row + column) % LANDSCAPE_USAGE_METER_SCROLL_BAND) *
    (LANDSCAPE_USAGE_METER_ANIMATION_STEPS /
      LANDSCAPE_USAGE_METER_SCROLL_BAND)
  );
  const uint8_t progress = static_cast<uint8_t>(
    (animationFrame + LANDSCAPE_USAGE_METER_ANIMATION_STEPS - phaseStep) %
    LANDSCAPE_USAGE_METER_ANIMATION_STEPS
  );
  if (progress == 0) return 100;
  if (progress <= 7) {
    return static_cast<uint8_t>(100 - ((72 * progress) / 7));
  }
  return progress == 15 ? 44 : 28;
}

inline uint16_t usageMeterDotColor(
  const UsageMeterRenderPlan& plan,
  uint16_t column,
  uint8_t row,
  bool animationActive,
  uint8_t animationFrame
) {
  if (column >= plan.dotFilledColumns) return plan.rects[0].color;
  if (!animationActive || plan.dotFilledColumns == 0) return plan.rects[1].color;

  const uint16_t gradientMaximum = plan.dotRows > 1
    ? plan.dotRows - 1
    : 0;
  const uint16_t gradientColor = usageMeterRgb565Interpolate(
    LANDSCAPE_USAGE_METER_ACTIVE,
    LANDSCAPE_USAGE_METER_BLUE_GREEN,
    row,
    gradientMaximum
  );
  return usageMeterRgb565Scale(
    gradientColor,
    usageMeterDiagonalBrightness(row, column, animationFrame)
  );
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

inline UsageMeterRenderPlan usageMeterLandscapeSinglePlan(
  const UsageMeterState& state,
  uint16_t fullWidth,
  uint16_t fullHeight
) {
  UsageMeterRenderPlan plan = {};
  if (!usageMeterStateValid(state) || fullWidth < USAGE_METER_MIN_WIDTH ||
      fullHeight < LANDSCAPE_USAGE_METER_FOOTPRINT) {
    return plan;
  }

  const uint16_t usableWidth = fullWidth - (USAGE_METER_SIDE_INSET * 2);
  const uint16_t y = fullHeight - LANDSCAPE_USAGE_METER_FOOTPRINT +
    LANDSCAPE_USAGE_METER_TOP_INSET;
  const uint16_t dotPitch = LANDSCAPE_USAGE_METER_DOT_SIZE +
    LANDSCAPE_USAGE_METER_DOT_GAP;
  const uint16_t dotColumns = (usableWidth + LANDSCAPE_USAGE_METER_DOT_GAP) /
    dotPitch;
  const bool useSevenDay = state.hasSevenDay;
  const uint8_t remaining = useSevenDay
    ? state.sevenDayRemaining
    : state.fiveHourRemaining;
  const uint16_t color = LANDSCAPE_USAGE_METER_ACTIVE;
  const uint16_t filledColumns = usageMeterFillWidth(dotColumns, remaining);
  const uint16_t filledWidth = filledColumns == 0
    ? 0
    : (filledColumns * LANDSCAPE_USAGE_METER_DOT_SIZE) +
      ((filledColumns - 1) * LANDSCAPE_USAGE_METER_DOT_GAP);

  plan.count = 2;
  plan.rects[0] = {
    USAGE_METER_SIDE_INSET,
    y,
    usableWidth,
    LANDSCAPE_USAGE_METER_GRID_HEIGHT,
    LANDSCAPE_USAGE_METER_CONSUMED,
  };
  plan.rects[1] = {
    USAGE_METER_SIDE_INSET,
    y,
    filledWidth,
    LANDSCAPE_USAGE_METER_GRID_HEIGHT,
    color,
  };
  plan.dotted = true;
  plan.dotSize = LANDSCAPE_USAGE_METER_DOT_SIZE;
  plan.dotGap = LANDSCAPE_USAGE_METER_DOT_GAP;
  plan.dotRows = LANDSCAPE_USAGE_METER_DOT_ROWS;
  plan.dotColumns = dotColumns;
  plan.dotFilledColumns = filledColumns;
  return plan;
}

inline UsageMeterRect usageMeterDotRect(
  const UsageMeterRenderPlan& plan,
  uint16_t column,
  uint8_t row
) {
  if (!plan.dotted || column >= plan.dotColumns || row >= plan.dotRows) {
    return {};
  }
  const uint16_t pitch = plan.dotSize + plan.dotGap;
  return {
    static_cast<uint16_t>(plan.rects[0].x + (column * pitch)),
    static_cast<uint16_t>(plan.rects[0].y + (row * pitch)),
    plan.dotSize,
    plan.dotSize,
    column < plan.dotFilledColumns ? plan.rects[1].color : plan.rects[0].color,
  };
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

inline UsageMeterRenderFrame usageMeterPrepareLandscapeSingleFrame(
  UsageMeterRenderState* renderState,
  bool connected,
  const UsageMeterState& usage,
  uint16_t fullWidth,
  uint16_t fullHeight,
  bool forceDraw = false,
  bool animationActive = false,
  uint8_t animationFrame = 0
) {
  UsageMeterRenderPlan plan = connected
    ? usageMeterLandscapeSinglePlan(usage, fullWidth, fullHeight)
    : UsageMeterRenderPlan{};
  const bool hasSevenDay = plan.count > 0 && usage.hasSevenDay;
  const bool hasFiveHour = plan.count > 0 && !hasSevenDay && usage.hasFiveHour;
  return {
    plan,
    usageMeterRenderTransition(
      renderState,
      plan.count > 0,
      hasFiveHour,
      hasSevenDay,
      hasFiveHour ? usage.fiveHourRemaining : 0,
      hasSevenDay ? usage.sevenDayRemaining : 0,
      forceDraw,
      animationActive && plan.dotFilledColumns > 0,
      animationFrame
    ),
  };
}
