#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "clock_time_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  ClockTimeFields fields = {};

  expect_true(clockFieldsFromLocalEpoch(0, &fields), "epoch conversion should succeed");
  expect_true(fields.year == 1970, "year should decode from epoch");
  expect_true(fields.month == 1, "month should decode from epoch");
  expect_true(fields.date == 1, "date should decode from epoch");
  expect_true(fields.week_day == 4, "weekday should decode from epoch");
  expect_true(fields.hours == 0, "hours should decode from epoch");
  expect_true(fields.minutes == 0, "minutes should decode from epoch");
  expect_true(fields.seconds == 0, "seconds should decode from epoch");

  expect_true(clockFieldsFromLocalEpoch(60 * 60 * 12 + 34 * 60 + 56, &fields), "second conversion should succeed");
  expect_true(fields.hours == 12, "hours should advance with epoch");
  expect_true(fields.minutes == 34, "minutes should advance with epoch");
  expect_true(fields.seconds == 56, "seconds should advance with epoch");

  ClockSyncEpochs sync = clockSyncEpochs(1710000000, 8 * 60 * 60);
  expect_true(sync.utc_epoch == 1710000000,
              "system UTC must use the raw bridge epoch");
  expect_true(sync.local_epoch == 1710000000 + 8 * 60 * 60,
              "display RTC should apply the timezone offset separately");

  return 0;
}
