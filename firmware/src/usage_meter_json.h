#pragma once

#include <ArduinoJson.h>

#include "usage_meter_logic.h"

inline void usageMeterApplyJson(JsonVariantConst usageVariant, UsageMeterState* state) {
  const bool usageObjectPresent = !usageVariant.isUnbound();
  JsonObjectConst usage = usageVariant.as<JsonObjectConst>();
  const bool usageObjectHasIntegerPair =
    usageObjectPresent && !usage.isNull() &&
    usage["five_hour_remaining"].is<int>() &&
    usage["seven_day_remaining"].is<int>();
  const int fiveHourRemaining = usageObjectHasIntegerPair
    ? usage["five_hour_remaining"].as<int>()
    : 0;
  const int sevenDayRemaining = usageObjectHasIntegerPair
    ? usage["seven_day_remaining"].as<int>()
    : 0;
  usageMeterApply(
    state,
    usageObjectPresent,
    usageObjectHasIntegerPair,
    fiveHourRemaining,
    sevenDayRemaining
  );
}
