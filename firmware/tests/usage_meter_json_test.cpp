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
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay, "integer JSON object should enable both windows");

  applyUsageJson("{}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay, "missing usage JSON member should preserve the live meter");
  expect_true(parsedUsage.fiveHourRemaining == 72, "missing usage JSON member should keep five-hour value");
  expect_true(parsedUsage.sevenDayRemaining == 91, "missing usage JSON member should keep weekly value");

  applyUsageJson("{\"usage\":null}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "null usage JSON member should preserve the last valid meter");
  expect_true(parsedUsage.fiveHourRemaining == 72 &&
              parsedUsage.sevenDayRemaining == 91,
              "null usage JSON member should retain both last valid values");

  applyUsageJson("{\"usage\":{\"seven_day_remaining\":99}}", &parsedUsage);
  expect_true(!parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "one valid weekly member should be accepted without synthesizing five hours");
  expect_true(parsedUsage.sevenDayRemaining == 99, "weekly-only JSON should retain its value");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72}}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && !parsedUsage.hasSevenDay,
              "one valid five-hour member should be accepted without synthesizing a week");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":\"not-an-object\"}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "malformed usage JSON should not erase the last valid meter");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72.5,\"seven_day_remaining\":91}}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "an invalid known value should preserve all last valid usage");

  applyUsageJson("{\"usage\":{\"seven_day_remaining\":101}}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "an out-of-range known value should preserve all last valid usage");

  applyUsageJson("{\"usage\":{\"future_window_remaining\":50}}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "an object without a known valid member should preserve the meter");

  applyUsageJson("{\"usage\":{\"five_hour_remaining\":72,\"seven_day_remaining\":91}}", &parsedUsage);
  applyUsageJson("{\"usage\":[72,91]}", &parsedUsage);
  expect_true(parsedUsage.hasFiveHour && parsedUsage.hasSevenDay,
              "array usage JSON member should preserve the last valid meter");

  return 0;
}
