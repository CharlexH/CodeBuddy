#pragma once

#include <stdint.h>

struct UsageMeterState {
  bool hasUsageLimits;
  uint8_t fiveHourRemaining;
  uint8_t sevenDayRemaining;
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
