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
    meterPlan.rects[0], 2, 224, 131, 6, USAGE_METER_CONSUMED,
    "five-hour bar should respect the side inset and sixteen-pixel footprint"
  );
  expectMeterRect(
    meterPlan.rects[1], 2, 224, 94, 6, USAGE_METER_FIVE_HOUR,
    "five-hour remaining should be a bright-green left fill"
  );
  expectMeterRect(
    meterPlan.rects[2], 2, 232, 131, 6, USAGE_METER_CONSUMED,
    "seven-day bar should follow the two-pixel inter-bar gap"
  );
  expectMeterRect(
    meterPlan.rects[3], 2, 232, 119, 6, USAGE_METER_SEVEN_DAY,
    "seven-day remaining should be a deep-green left fill"
  );

  UsageMeterRenderPlan landscapePlan = usageMeterRenderPlan(meterUsage, 240, 135);
  expect_true(landscapePlan.count == 4, "landscape usage should use the same four drawing operations");
  expectMeterRect(
    landscapePlan.rects[0], 2, 119, 236, 6, USAGE_METER_CONSUMED,
    "landscape five-hour bar should use the full inset width"
  );
  expectMeterRect(
    landscapePlan.rects[1], 2, 119, 169, 6, USAGE_METER_FIVE_HOUR,
    "landscape five-hour fill should use the inset width"
  );
  expectMeterRect(
    landscapePlan.rects[2], 2, 127, 236, 6, USAGE_METER_CONSUMED,
    "landscape seven-day bar should preserve the gap and bottom margin"
  );
  expectMeterRect(
    landscapePlan.rects[3], 2, 127, 214, 6, USAGE_METER_SEVEN_DAY,
    "landscape seven-day fill should use the inset width"
  );

  expect_true(
    usageMeterRenderPlan(meterUsage, 4, 240).count == 0,
    "surfaces narrower than five pixels should not render a usage meter"
  );
  expect_true(
    usageMeterRenderPlan(meterUsage, 135, 15).count == 0,
    "surfaces shorter than the sixteen-pixel footprint should not render a usage meter"
  );

  UsageMeterState noMeterUsage = {false, 72, 91};
  expect_true(
    usageMeterRenderPlan(noMeterUsage, 135, 240).count == 0,
    "invalid or unavailable usage should yield no drawing operations"
  );
  expect_true(
    usageMeterFooterInset(true, meterUsage) == 16,
    "visible usage should reserve the complete sixteen-pixel footprint above approval footer text"
  );
  expect_true(
    usageMeterFooterInset(false, meterUsage) == 0,
    "disconnected usage should not move the approval footer"
  );
  expect_true(
    usageMeterFooterInset(true, noMeterUsage) == 0,
    "invalid usage should not move the approval footer"
  );

  UsageMeterRenderState portraitState = {};
  UsageMeterRenderFrame portraitVisible = usageMeterPrepareFrame(
    &portraitState, true, meterUsage, 135, 240
  );
  expect_true(portraitVisible.decision.draw && !portraitVisible.decision.clear,
              "a visible portrait meter should paint without first clearing the sprite");
  expect_true(portraitVisible.plan.count == 4,
              "a visible portrait meter should retain its full sixteen-pixel plan");

  UsageMeterRenderFrame portraitHidden = usageMeterPrepareFrame(
    &portraitState, true, noMeterUsage, 135, 240
  );
  expect_true(!portraitHidden.decision.draw && portraitHidden.decision.clear,
              "a hidden portrait meter should clear the stale strip before other sprite content");
  expect_true(portraitHidden.plan.count == 0,
              "a hidden portrait meter should not repaint any meter rectangles");

  UsageMeterRenderFrame portraitStillHidden = usageMeterPrepareFrame(
    &portraitState, true, noMeterUsage, 135, 240
  );
  expect_true(!portraitStillHidden.decision.draw && !portraitStillHidden.decision.clear,
              "a hidden portrait meter should not repeatedly clear the sprite");

  UsageMeterRenderState landscapeState = {};
  UsageMeterRenderDecision firstVisible = usageMeterRenderTransition(&landscapeState, true);
  expect_true(firstVisible.draw && !firstVisible.clear,
              "a newly visible meter should draw without clearing its background");

  UsageMeterRenderDecision stillVisible = usageMeterRenderTransition(&landscapeState, true);
  expect_true(!stillVisible.draw && !stillVisible.clear,
              "an unchanged visible direct meter should not redraw every frame");

  UsageMeterState changedMeterUsage = {true, 71, 91};
  UsageMeterRenderFrame changedVisible = usageMeterPrepareFrame(
    &landscapeState, true, changedMeterUsage, 240, 135
  );
  expect_true(changedVisible.decision.draw && !changedVisible.decision.clear,
              "a changed direct meter limit should redraw the strip");

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
