#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clock_display_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static void expect_str_eq(const char* actual, const char* expected, const char* message) {
  if (strcmp(actual, expected) != 0) {
    fprintf(stderr, "%s: expected '%s' got '%s'\n", message, expected, actual);
    exit(1);
  }
}

int main() {
  char buf[16];
  char hm[6];
  char seconds[4];

  clockFormatHm(buf, sizeof(buf), 9, 5);
  expect_str_eq(buf, "09:05", "valid hours and minutes should render normally");

  clockFormatHm(buf, sizeof(buf), -1, 42);
  expect_str_eq(buf, "--:--", "invalid hour should render as placeholder instead of unsigned garbage");

  clockFormatSeconds(buf, sizeof(buf), 42);
  expect_str_eq(buf, ":42", "valid seconds should render normally");

  clockFormatSeconds(buf, sizeof(buf), -1);
  expect_str_eq(buf, ":--", "invalid seconds should render as placeholder");

  clockFormatDateLine(buf, sizeof(buf), 4, 20);
  expect_str_eq(buf, "Apr 20", "valid month and date should render normally");

  clockFormatDateLine(buf, sizeof(buf), -1, 20);
  expect_str_eq(buf, "--- --", "invalid month should render as placeholder");

  clockFormatSharedTimeSegments(hm, sizeof(hm), seconds, sizeof(seconds), true, 9, 5, 7);
  expect_str_eq(hm, "09:05", "shared face should retain HH:MM as the primary segment");
  expect_str_eq(seconds, ":07", "shared face should retain :SS as the dim segment");

  clockFormatSharedTimeSegments(hm, sizeof(hm), seconds, sizeof(seconds), false, 9, 5, 7);
  expect_str_eq(hm, "--:--", "an invalid shared-face clock should replace the whole primary segment");
  expect_str_eq(seconds, ":--", "an invalid shared-face clock should replace the seconds segment too");

  clockFormatSharedDateLine(buf, sizeof(buf), false, true, 6, 4, 20);
  expect_str_eq(buf, "Apr 20", "the portrait shared face should retain its guarded date line");
  clockFormatSharedDateLine(buf, sizeof(buf), true, true, 6, 4, 20);
  expect_str_eq(buf, "Sat Apr 20", "the landscape shared face should retain its weekday and date");
  clockFormatSharedDateLine(buf, sizeof(buf), true, false, 6, 4, 20);
  expect_str_eq(buf, "--- --- --", "an invalid shared-face date should use a fixed-width placeholder");

  expect_true(clockSharedFieldsValid(23, 59, 59, 6, 12, 31),
              "guarded host or RTC fields should be accepted when every clock field is valid");
  expect_true(!clockSharedFieldsValid(23, 59, 60, 6, 12, 31),
              "invalid seconds should invalidate the complete shared clock value");
  expect_true(!clockSharedFieldsValid(23, 59, 59, 7, 12, 31),
              "invalid weekdays should invalidate the retained shared date");

  SharedClockFaceCache standby = {};
  SharedClockFaceCache runtime = {};
  SharedClockFaceRenderDecision sharedDecision = clockSharedFaceSchedule(
    &standby, 1000, 0, 10, 6, 4, 20, 0, false, false, false, false
  );
  expect_true(sharedDecision.fullRepaint && sharedDecision.drawTime &&
                  sharedDecision.drawDate && sharedDecision.drawPet && sharedDecision.drawMeters,
              "the first standby frame should paint every shared-face layer");
  expect_true(!runtime.initialized,
              "updating the standby cache must not initialize or mutate the runtime cache");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 1000, 0, 10, 6, 4, 20, 2, false, false, false, false
  );
  expect_true(sharedDecision.fullRepaint,
              "the runtime cache should independently request its own first-entry repaint");
  expect_true(standby.initialized && runtime.initialized,
              "standby and runtime should own explicit independent render caches");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 1100, 0, 10, 6, 4, 20, 2, false, false, false, false
  );
  expect_true(!sharedDecision.fullRepaint && !sharedDecision.drawTime &&
                  !sharedDecision.drawDate && !sharedDecision.drawPet,
              "an unchanged frame before 200ms should not redraw shared-face layers");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 1200, 0, 10, 6, 4, 20, 2, false, false, false, false
  );
  expect_true(!sharedDecision.fullRepaint && sharedDecision.drawPet &&
                  !sharedDecision.drawTime && !sharedDecision.drawDate,
              "the compact pet should update at 5fps without touching clock text");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 2000, 0, 11, 6, 4, 20, 2, false, false, false, false
  );
  expect_true(!sharedDecision.fullRepaint && sharedDecision.drawTime,
              "a one-second change should update the one-line clock without a full clear");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 2005, 0, 11, 0, 4, 21, 2, false, false, false, false
  );
  expect_true(!sharedDecision.fullRepaint && sharedDecision.drawDate &&
                  !sharedDecision.drawTime && !sharedDecision.drawPet,
              "a date rollover should update the retained date without disturbing other layers");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 2010, 1, 11, 0, 4, 21, 2, false, false, false, false
  );
  expect_true(sharedDecision.fullRepaint && sharedDecision.clearSurface,
              "an orientation switch should cleanly repaint the complete shared face");

  sharedDecision = clockSharedFaceSchedule(
    &runtime, 2020, 1, 11, 0, 4, 21, 2, false, true, false, false
  );
  expect_true(sharedDecision.fullRepaint && sharedDecision.clearSurface,
              "returning from approval should cleanly repaint the complete shared face");

  return 0;
}
