#include <stdio.h>
#include <stdlib.h>

#include <ArduinoJson.h>
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
