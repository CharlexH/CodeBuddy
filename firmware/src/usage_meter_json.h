#pragma once

#include <ArduinoJson.h>

#include "usage_meter_logic.h"

inline void usageMeterApplyJson(JsonVariantConst usageVariant, UsageMeterState* state) {
  const bool usageObjectPresent = !usageVariant.isUnbound();
  JsonObjectConst usage = usageVariant.as<JsonObjectConst>();
  const bool isObject = usageObjectPresent && !usage.isNull();
  JsonVariantConst fiveHour = usage["five_hour_remaining"];
  JsonVariantConst sevenDay = usage["seven_day_remaining"];
  const bool hasFiveHour = isObject && !fiveHour.isUnbound();
  const bool hasSevenDay = isObject && !sevenDay.isUnbound();
  const bool fiveHourTyped = !hasFiveHour || fiveHour.is<int>();
  const bool sevenDayTyped = !hasSevenDay || sevenDay.is<int>();
  const bool usageObjectValid = isObject && (hasFiveHour || hasSevenDay) &&
    fiveHourTyped && sevenDayTyped;
  const int fiveHourRemaining = hasFiveHour && fiveHourTyped
    ? fiveHour.as<int>()
    : 0;
  const int sevenDayRemaining = hasSevenDay && sevenDayTyped
    ? sevenDay.as<int>()
    : 0;
  usageMeterApply(
    state,
    usageObjectPresent,
    usageObjectValid,
    hasFiveHour,
    fiveHourRemaining,
    hasSevenDay,
    sevenDayRemaining
  );
}
