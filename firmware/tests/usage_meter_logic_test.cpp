#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>
#include "usage_meter_logic.h"
#include "usage_meter_json.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static void applyUsageJson(const char* payload, UsageMeterState* usage) {
  JsonDocument doc;
  expect_true(!deserializeJson(doc, payload), "usage test JSON should deserialize");
  usageMeterApplyJson(doc["usage"], usage);
}

int main() {
  UsageMeterState usage = {};

  expect_true(usageMeterValidPair(0, 100), "0 and 100 percent should be valid");
  expect_true(usageMeterValidPair(72, 91), "in-range percentage pair should be valid");
  expect_true(!usageMeterValidPair(-1, 91), "negative five-hour value should be invalid");
  expect_true(!usageMeterValidPair(72, 101), "weekly value above 100 should be invalid");

  usageMeterApply(&usage, true, true, 72, 91);
  expect_true(usage.hasUsageLimits, "complete valid object should enable the meter");
  expect_true(usage.fiveHourRemaining == 72, "complete valid object should retain five-hour value");
  expect_true(usage.sevenDayRemaining == 91, "complete valid object should retain weekly value");

  usageMeterApply(&usage, false, false, 0, 0);
  expect_true(usage.hasUsageLimits, "absent usage object should preserve prior value");
  expect_true(usage.fiveHourRemaining == 72, "absent usage object should not overwrite five-hour value");
  expect_true(usage.sevenDayRemaining == 91, "absent usage object should not overwrite weekly value");

  usageMeterApply(&usage, true, false, 0, 0);
  expect_true(!usage.hasUsageLimits, "malformed present object must clear the whole meter");

  usageMeterApply(&usage, true, true, 72, 101);
  expect_true(!usage.hasUsageLimits, "out-of-range present object must clear the whole meter");

  usageMeterApply(&usage, true, true, 72, 91);
  usageMeterClear(&usage);
  expect_true(!usage.hasUsageLimits, "disconnect should clear the meter instead of showing stale limits");
  expect_true(usage.fiveHourRemaining == 0, "disconnect should clear stale five-hour value");
  expect_true(usage.sevenDayRemaining == 0, "disconnect should clear stale weekly value");

  expect_true(usageMeterFillWidth(135, 0) == 0, "zero percent should have zero fill width");
  expect_true(usageMeterFillWidth(135, 100) == 135, "100 percent should fill the full width");
  expect_true(usageMeterFillWidth(135, 72) == 97, "fractional pixel widths should truncate deterministically");
  expect_true(usageMeterFillWidth(135, 99) == 133, "fractional pixel widths should use integer math");

  UsageMeterState parsedUsage = {};
  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  expect_true(parsedUsage.hasUsageLimits, "integer JSON object should enable the meter");

  applyUsageJson("{}", &parsedUsage);
  expect_true(parsedUsage.hasUsageLimits, "missing usage JSON member should preserve the live meter");
  expect_true(parsedUsage.fiveHourRemaining == 72, "missing usage JSON member should keep five-hour value");
  expect_true(parsedUsage.sevenDayRemaining == 91, "missing usage JSON member should keep weekly value");

  applyUsageJson("{\"usage\":null}", &parsedUsage);
  expect_true(!parsedUsage.hasUsageLimits, "null usage JSON member should clear the meter");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":\"not-an-object\"}", &parsedUsage);
  expect_true(!parsedUsage.hasUsageLimits, "string usage JSON member should clear the meter");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72.5,\"seven_day_remaining\":91}}", &parsedUsage);
  expect_true(!parsedUsage.hasUsageLimits, "fractional usage JSON value should clear the meter");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":[72,91]}", &parsedUsage);
  expect_true(!parsedUsage.hasUsageLimits, "array usage JSON member should clear the meter");

  return 0;
}
