#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "shared_clock_face_logic.h"

static const char CLOCK_MON[][4] = {
  "JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"
};

static const char CLOCK_DOW[][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

inline bool clockValueInRange(int value, int min_value, int max_value) {
  return value >= min_value && value <= max_value;
}

inline const char* clockMonthLabel(int month) {
  return clockValueInRange(month, 1, 12) ? CLOCK_MON[month - 1] : "---";
}

inline const char* clockWeekdayLabel(int weekday) {
  return clockValueInRange(weekday, 0, 6) ? CLOCK_DOW[weekday] : "---";
}

inline void clockFormatHm(char* out, size_t size, int hours, int minutes) {
  if (!clockValueInRange(hours, 0, 23) || !clockValueInRange(minutes, 0, 59)) {
    snprintf(out, size, "--:--");
    return;
  }
  snprintf(out, size, "%02d:%02d", hours, minutes);
}

inline void clockFormatSeconds(char* out, size_t size, int seconds) {
  if (!clockValueInRange(seconds, 0, 59)) {
    snprintf(out, size, ":--");
    return;
  }
  snprintf(out, size, ":%02d", seconds);
}

inline void clockFormatDateLine(char* out, size_t size, int month, int date) {
  if (!clockValueInRange(month, 1, 12) || !clockValueInRange(date, 1, 31)) {
    snprintf(out, size, "--- --");
    return;
  }
  snprintf(out, size, "%s %02d", clockMonthLabel(month), date);
}

inline void clockFormatWeekDateLine(char* out, size_t size, int weekday, int month, int date) {
  if (!clockValueInRange(month, 1, 12) || !clockValueInRange(date, 1, 31)) {
    snprintf(out, size, "%s --- --", clockWeekdayLabel(weekday));
    return;
  }
  snprintf(out, size, "%s %s %02d", clockWeekdayLabel(weekday), clockMonthLabel(month), date);
}

inline bool clockSharedFieldsValid(
  bool timeTrusted,
  int hours,
  int minutes,
  int seconds,
  int weekday,
  int month,
  int date
) {
  return timeTrusted &&
    clockValueInRange(hours, 0, 23) &&
    clockValueInRange(minutes, 0, 59) &&
    clockValueInRange(seconds, 0, 59) &&
    clockValueInRange(weekday, 0, 6) &&
    clockValueInRange(month, 1, 12) &&
    clockValueInRange(date, 1, 31);
}

inline void clockFormatSharedTimeSegments(
  char* hm,
  size_t hmSize,
  char* seconds,
  size_t secondsSize,
  bool timeValid,
  int hours,
  int minutes,
  int second
) {
  if (!timeValid) {
    snprintf(hm, hmSize, "--:--");
    snprintf(seconds, secondsSize, ":--");
    return;
  }
  clockFormatHm(hm, hmSize, hours, minutes);
  clockFormatSeconds(seconds, secondsSize, second);
}

inline void clockFormatSharedDateLine(
  char* out,
  size_t size,
  bool includeWeekday,
  bool dateValid,
  int weekday,
  int month,
  int date
) {
  if (!dateValid) {
    snprintf(out, size, includeWeekday ? "--- --- --" : "--- --");
    return;
  }
  if (includeWeekday) clockFormatWeekDateLine(out, size, weekday, month, date);
  else clockFormatDateLine(out, size, month, date);
}

struct SharedClockFaceCache {
  bool initialized;
  uint8_t orientation;
  int16_t lastSecond;
  uint16_t lastDateKey;
  uint8_t lastPersona;
  SharedClockStatusCounts lastStatus;
  uint32_t nextPetFrameAt;
};

inline void clockSharedFaceCacheReset(SharedClockFaceCache* cache) {
  if (cache == nullptr) return;
  *cache = {};
}

inline uint16_t clockSharedDateKey(int weekday, int month, int date) {
  return (uint16_t)(((weekday & 0x07) << 9) | ((month & 0x0F) << 5) | (date & 0x1F));
}

inline SharedClockFaceRenderDecision clockSharedFaceSchedule(
  SharedClockFaceCache* cache,
  uint32_t now,
  uint8_t orientation,
  bool statusVisible,
  SharedClockStatusCounts status,
  int second,
  int weekday,
  int month,
  int date,
  uint8_t persona,
  bool forceFullRepaint,
  bool promptExited,
  bool metersChanged,
  bool forceMeters
) {
  if (cache == nullptr) return {};

  const bool firstEntry = !cache->initialized;
  const bool orientationChanged = cache->initialized && cache->orientation != orientation;
  const uint16_t dateKey = clockSharedDateKey(weekday, month, date);
  const bool secondChanged = cache->initialized && cache->lastSecond != second;
  const bool dateChanged = cache->initialized && cache->lastDateKey != dateKey;
  const bool personaChanged = cache->initialized && cache->lastPersona != persona;
  const bool statusChanged = cache->initialized &&
    (cache->lastStatus.running != status.running ||
      cache->lastStatus.waiting != status.waiting ||
      cache->lastStatus.unread != status.unread);
  const bool petFrameDue = firstEntry || personaChanged ||
    sharedClockPetFrameDue(now, cache->nextPetFrameAt);

  SharedClockFaceRenderInput input = {};
  input.firstEntry = firstEntry;
  input.orientationChanged = orientationChanged;
  // Character state changes may reopen a GIF by clearing the portrait
  // sprite, so repaint the shared text layers in the same transaction.
  input.fullRepaintRequested = forceFullRepaint || personaChanged;
  input.secondChanged = secondChanged;
  input.dateChanged = dateChanged;
  input.petFrameDue = petFrameDue;
  input.statusVisible = statusVisible;
  input.statusChanged = statusChanged;
  input.metersChanged = metersChanged;
  input.forceMeters = forceMeters;
  input.promptExited = promptExited;
  SharedClockFaceRenderDecision decision = sharedClockFaceRenderDecision(input);

  cache->initialized = true;
  cache->orientation = orientation;
  cache->lastSecond = second;
  cache->lastDateKey = dateKey;
  cache->lastPersona = persona;
  cache->lastStatus = status;
  if (decision.drawPet) cache->nextPetFrameAt = now + SHARED_CLOCK_PET_FRAME_INTERVAL_MS;
  return decision;
}
