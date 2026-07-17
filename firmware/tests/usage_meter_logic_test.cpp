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

  expect_true(usageMeterValidPercent(0), "zero percent should be valid");
  expect_true(usageMeterValidPercent(100), "one hundred percent should be valid");
  expect_true(!usageMeterValidPercent(-1), "negative values should be invalid");
  expect_true(!usageMeterValidPercent(101), "values above 100 should be invalid");

  usageMeterApply(&usage, true, true, true, 72, true, 91);
  expect_true(usage.hasFiveHour && usage.hasSevenDay, "complete valid object should enable both windows");
  expect_true(usage.fiveHourRemaining == 72, "complete valid object should retain five-hour value");
  expect_true(usage.sevenDayRemaining == 91, "complete valid object should retain weekly value");

  usageMeterApply(&usage, false, false, false, 0, false, 0);
  expect_true(usage.hasFiveHour && usage.hasSevenDay, "absent usage object should preserve prior value");
  expect_true(usage.fiveHourRemaining == 72, "absent usage object should not overwrite five-hour value");
  expect_true(usage.sevenDayRemaining == 91, "absent usage object should not overwrite weekly value");

  usageMeterApply(&usage, true, true, false, 0, true, 99);
  expect_true(!usage.hasFiveHour && usage.hasSevenDay, "weekly-only object should retain only the weekly window");
  expect_true(usage.sevenDayRemaining == 99, "weekly-only object should retain its value");

  usageMeterApply(&usage, true, true, true, 72, false, 0);
  expect_true(usage.hasFiveHour && !usage.hasSevenDay, "five-hour-only object should retain only that window");

  usageMeterApply(&usage, true, false, false, 0, false, 0);
  expect_true(!usage.hasFiveHour && !usage.hasSevenDay, "malformed present object must clear the whole meter");

  usageMeterApply(&usage, true, true, true, 72, true, 101);
  expect_true(!usage.hasFiveHour && !usage.hasSevenDay, "out-of-range known value must clear the whole meter");

  usageMeterApply(&usage, true, true, true, 72, true, 91);
  usageMeterClear(&usage);
  expect_true(!usage.hasFiveHour && !usage.hasSevenDay, "disconnect should clear the meter instead of showing stale limits");
  expect_true(usage.fiveHourRemaining == 0, "disconnect should clear stale five-hour value");
  expect_true(usage.sevenDayRemaining == 0, "disconnect should clear stale weekly value");

  expect_true(usageMeterFillWidth(135, 0) == 0, "zero percent should have zero fill width");
  expect_true(usageMeterFillWidth(135, 100) == 135, "100 percent should fill the full width");
  expect_true(usageMeterFillWidth(135, 72) == 97, "fractional pixel widths should truncate deterministically");
  expect_true(usageMeterFillWidth(135, 99) == 133, "fractional pixel widths should use integer math");

  UsageMeterState meterUsage = {true, true, 72, 91};
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

  UsageMeterRenderPlan dashboardPlan = usageMeterLandscapeSinglePlan(meterUsage, 240, 135);
  expect_true(dashboardPlan.count == 2,
              "landscape dashboard should render one base and one fill");
  expect_true(dashboardPlan.dotted && dashboardPlan.dotSize == 2 &&
                  dashboardPlan.dotGap == 2,
              "landscape dashboard should use two-pixel dots with two-pixel gaps");
  expect_true(dashboardPlan.dotColumns == 59 && dashboardPlan.dotRows == 5,
              "landscape dashboard should fit a 59 by 5 dot matrix");
  expect_true(dashboardPlan.dotFilledColumns == 53,
              "91 percent weekly remaining should fill 53 of 59 columns");
  expectMeterRect(
    dashboardPlan.rects[0], 2, 116, 236, 18, USAGE_METER_CONSUMED,
    "landscape dashboard bounds should center the dot matrix in the 20-pixel footer"
  );
  expect_true(dashboardPlan.rects[1].color == USAGE_METER_SEVEN_DAY,
              "landscape dashboard should prefer the seven-day quota color");
  expectMeterRect(
    usageMeterDotRect(dashboardPlan, 0, 0), 2, 116, 2, 2,
    USAGE_METER_SEVEN_DAY,
    "the first filled dot should start at the left and top grid inset"
  );
  expectMeterRect(
    usageMeterDotRect(dashboardPlan, 53, 0), 214, 116, 2, 2,
    USAGE_METER_CONSUMED,
    "the first unfilled dot should use the consumed color after the weekly fill"
  );
  expectMeterRect(
    usageMeterDotRect(dashboardPlan, 58, 4), 234, 132, 2, 2,
    USAGE_METER_CONSUMED,
    "the final dot should preserve the centered footer margins"
  );

  expect_true(
    usageMeterRenderPlan(meterUsage, 4, 240).count == 0,
    "surfaces narrower than five pixels should not render a usage meter"
  );
  expect_true(
    usageMeterRenderPlan(meterUsage, 135, 15).count == 0,
    "surfaces shorter than the sixteen-pixel footprint should not render a usage meter"
  );

  UsageMeterState noMeterUsage = {false, false, 72, 91};
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

  UsageMeterState weekOnlyUsage = {false, true, 0, 99};
  UsageMeterRenderPlan weekOnlyPortrait = usageMeterRenderPlan(weekOnlyUsage, 135, 240);
  expect_true(weekOnlyPortrait.count == 2, "weekly-only usage should compose one base and one fill");
  expectMeterRect(
    weekOnlyPortrait.rects[0], 2, 232, 131, 6, USAGE_METER_CONSUMED,
    "weekly-only portrait bar should sit two pixels above the bottom"
  );
  expectMeterRect(
    weekOnlyPortrait.rects[1], 2, 232, 129, 6, USAGE_METER_SEVEN_DAY,
    "weekly-only portrait fill should truncate 131 times 99 percent to 129 pixels"
  );
  UsageMeterRenderPlan weekOnlyLandscape = usageMeterRenderPlan(weekOnlyUsage, 240, 135);
  expectMeterRect(
    weekOnlyLandscape.rects[0], 2, 127, 236, 6, USAGE_METER_CONSUMED,
    "weekly-only landscape bar should sit two pixels above the bottom"
  );
  expectMeterRect(
    weekOnlyLandscape.rects[1], 2, 127, 233, 6, USAGE_METER_SEVEN_DAY,
    "weekly-only landscape fill should truncate 236 times 99 percent to 233 pixels"
  );
  expect_true(usageMeterFooterInset(true, weekOnlyUsage) == 8,
              "one visible window should reserve an eight-pixel footer footprint");
  expect_true(usageMeterRenderPlan(weekOnlyUsage, 135, 7).count == 0,
              "a single meter needs the complete eight-pixel footprint");

  UsageMeterState fiveOnlyUsage = {true, false, 99, 0};
  UsageMeterRenderPlan fiveOnlyPlan = usageMeterRenderPlan(fiveOnlyUsage, 135, 240);
  expectMeterRect(
    fiveOnlyPlan.rects[1], 2, 232, 129, 6, USAGE_METER_FIVE_HOUR,
    "five-hour-only usage should use the bright-green single bar"
  );
  UsageMeterRenderPlan fiveOnlyDashboard = usageMeterLandscapeSinglePlan(fiveOnlyUsage, 240, 135);
  expect_true(fiveOnlyDashboard.dotFilledColumns == 58 &&
                  fiveOnlyDashboard.rects[1].color == USAGE_METER_FIVE_HOUR,
              "landscape dashboard should fall back to five-hour dots when weekly is absent");
  expect_true(
    usageMeterLandscapeSinglePlan(noMeterUsage, 240, 135).count == 0,
    "landscape dashboard should omit an unavailable meter"
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
  UsageMeterRenderState dashboardState = {};
  UsageMeterRenderFrame dashboardVisible = usageMeterPrepareLandscapeSingleFrame(
    &dashboardState, true, meterUsage, 240, 135
  );
  expect_true(dashboardVisible.decision.draw && !dashboardVisible.decision.clear &&
                  dashboardVisible.plan.count == 2,
              "landscape dashboard frame should paint its selected single meter");
  UsageMeterState fiveHourChangedOnly = {true, true, 50, 91};
  UsageMeterRenderFrame dashboardUnchanged = usageMeterPrepareLandscapeSingleFrame(
    &dashboardState, true, fiveHourChangedOnly, 240, 135
  );
  expect_true(!dashboardUnchanged.decision.draw && !dashboardUnchanged.decision.clear,
              "an unselected five-hour change should not redraw the weekly dashboard bar");

  UsageMeterRenderDecision firstVisible = usageMeterRenderTransition(
    &landscapeState, true, true, true, 72, 91
  );
  expect_true(firstVisible.draw && !firstVisible.clear,
              "a newly visible meter should draw without clearing its background");

  UsageMeterRenderDecision stillVisible = usageMeterRenderTransition(
    &landscapeState, true, true, true, 72, 91
  );
  expect_true(!stillVisible.draw && !stillVisible.clear,
              "an unchanged visible direct meter should not redraw every frame");

  UsageMeterState changedMeterUsage = {true, true, 71, 91};
  UsageMeterRenderFrame changedVisible = usageMeterPrepareFrame(
    &landscapeState, true, changedMeterUsage, 240, 135
  );
  expect_true(changedVisible.decision.draw && !changedVisible.decision.clear,
              "a changed direct meter limit should redraw the strip");

  UsageMeterRenderFrame changedToSingle = usageMeterPrepareFrame(
    &landscapeState, true, weekOnlyUsage, 240, 135
  );
  expect_true(changedToSingle.decision.draw && changedToSingle.decision.clear,
              "dual-to-single transition should clear the old sixteen-pixel strip and repaint once");

  UsageMeterRenderFrame stableSingle = usageMeterPrepareFrame(
    &landscapeState, true, weekOnlyUsage, 240, 135
  );
  expect_true(!stableSingle.decision.draw && !stableSingle.decision.clear,
              "an unchanged single meter should not redraw or clear repeatedly");

  UsageMeterRenderDecision hidden = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!hidden.draw && hidden.clear,
              "a meter that becomes unavailable should clear its previous strip once");

  UsageMeterRenderDecision stillHidden = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!stillHidden.draw && !stillHidden.clear,
              "an already-cleared meter should not repeatedly erase the landscape surface");

  usageMeterRenderTransition(&landscapeState, true, true, true, 72, 91);
  usageMeterRenderReset(&landscapeState);
  UsageMeterRenderDecision hiddenAfterLandscapeReset = usageMeterRenderTransition(&landscapeState, false);
  expect_true(!hiddenAfterLandscapeReset.draw && !hiddenAfterLandscapeReset.clear,
              "a fresh direct landscape surface should not clear a meter from an earlier surface");

  return 0;
}
