#pragma once

#include <stdint.h>
#include <time.h>

struct ClockTimeFields {
  int16_t year;
  int8_t month;
  int8_t date;
  int8_t week_day;
  int8_t hours;
  int8_t minutes;
  int8_t seconds;
};

struct ClockSyncEpochs {
  int64_t utc_epoch;
  int64_t local_epoch;
};

inline constexpr ClockSyncEpochs clockSyncEpochs(
  int64_t utc_epoch,
  int32_t timezone_offset_seconds
) {
  return {utc_epoch, utc_epoch + timezone_offset_seconds};
}

inline bool clockFieldsFromLocalEpoch(int64_t local_epoch, ClockTimeFields* out) {
  if (!out) return false;
  time_t raw = (time_t)local_epoch;
  struct tm local_tm;
  if (!gmtime_r(&raw, &local_tm)) return false;
  out->year = (int16_t)(local_tm.tm_year + 1900);
  out->month = (int8_t)(local_tm.tm_mon + 1);
  out->date = (int8_t)local_tm.tm_mday;
  out->week_day = (int8_t)local_tm.tm_wday;
  out->hours = (int8_t)local_tm.tm_hour;
  out->minutes = (int8_t)local_tm.tm_min;
  out->seconds = (int8_t)local_tm.tm_sec;
  return true;
}
