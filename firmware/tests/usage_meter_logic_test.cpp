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

static void expectMeterRect(
  const UsageMeterRect& actual,
  uint16_t x,
  uint16_t y,
  uint16_t width,
  uint8_t height,
  uint16_t color,
  const char* message
) {
  expect_true(
    actual.x == x && actual.y == y && actual.width == width &&
    actual.height == height && actual.color == color,
    message
  );
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

  UsageMeterState meterUsage = {true, 72, 91};
  UsageMeterRenderPlan meterPlan = usageMeterRenderPlan(meterUsage, 135, 240);
  expect_true(meterPlan.count == 4, "valid usage should compose two base and two fill operations");
  expectMeterRect(
    meterPlan.rects[0], 0, 234, 135, 3, USAGE_METER_CONSUMED,
    "five-hour lane should start flush at the top of the six-pixel meter"
  );
  expectMeterRect(
    meterPlan.rects[1], 0, 234, 97, 3, USAGE_METER_FIVE_HOUR,
    "five-hour remaining should be a bright-green left fill"
  );
  expectMeterRect(
    meterPlan.rects[2], 0, 237, 135, 3, USAGE_METER_CONSUMED,
    "seven-day lane should start flush at the bottom half of the meter"
  );
  expectMeterRect(
    meterPlan.rects[3], 0, 237, 122, 3, USAGE_METER_SEVEN_DAY,
    "seven-day remaining should be a deep-green left fill"
  );

  UsageMeterState noMeterUsage = {false, 72, 91};
  expect_true(
    usageMeterRenderPlan(noMeterUsage, 135, 240).count == 0,
    "invalid or unavailable usage should yield no drawing operations"
  );
  expect_true(
    usageMeterFooterInset(true, meterUsage) == USAGE_METER_HEIGHT,
    "visible usage should reserve the meter height above approval footer text"
  );
  expect_true(
    usageMeterFooterInset(false, meterUsage) == 0,
    "disconnected usage should not move the approval footer"
  );
  expect_true(
    usageMeterFooterInset(true, noMeterUsage) == 0,
    "invalid usage should not move the approval footer"
  );

  UsageMeterRenderState landscapeState = {};
  UsageMeterRenderDecision firstVisible = usageMeterRenderTransition(&landscapeState, true);
  expect_true(firstVisible.draw && !firstVisible.clear,
              "a newly visible meter should draw without clearing its background");

  UsageMeterRenderDecision stillVisible = usageMeterRenderTransition(&landscapeState, true);
  expect_true(stillVisible.draw && !stillVisible.clear,
              "a visible meter should keep drawing without clearing its background");

  UsageMeterRenderDecision hidden = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!hidden.draw && hidden.clear,
              "a meter that becomes unavailable should clear its previous strip once");

  UsageMeterRenderDecision stillHidden = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!stillHidden.draw && !stillHidden.clear,
              "an already-cleared meter should not repeatedly erase the landscape surface");

  usageMeterRenderTransition(&landscapeState, true);
  usageMeterRenderReset(&landscapeState);
  UsageMeterRenderDecision hiddenAfterLandscapeReset = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!hiddenAfterLandscapeReset.draw && !hiddenAfterLandscapeReset.clear,
              "a fresh direct landscape surface should not clear a meter from an earlier surface");

  return 0;
}
