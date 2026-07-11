#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "usage_meter_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
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

  expect_true(usageMeterFillWidth(135, 0) == 0, "zero percent should have zero fill width");
  expect_true(usageMeterFillWidth(135, 100) == 135, "100 percent should fill the full width");
  expect_true(usageMeterFillWidth(135, 72) == 97, "fractional pixel widths should truncate deterministically");
  expect_true(usageMeterFillWidth(135, 99) == 133, "fractional pixel widths should use integer math");

  return 0;
}
