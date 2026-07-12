#pragma once

#include <stdint.h>

enum TrustedTimeSource : uint8_t {
  TRUSTED_TIME_NONE,
  TRUSTED_TIME_SECURE_HOST,
  TRUSTED_TIME_SNTP,
};

struct TrustedTimeState {
  TrustedTimeSource source;
  int64_t epochAtSync;
  uint32_t syncAtMs;
};

static constexpr int64_t TRUSTED_TIME_MIN_EPOCH = 1700000000LL;
static constexpr int64_t TRUSTED_TIME_MAX_EPOCH = 4102444800LL;
static constexpr int32_t TRUSTED_TIME_MAX_TZ_OFFSET = 14 * 60 * 60;
static constexpr uint32_t TRUSTED_TIME_MAX_AGE_MS = 24UL * 60UL * 60UL * 1000UL;

inline constexpr TrustedTimeState trustedTimeInitial() {
  return {TRUSTED_TIME_NONE, 0, 0};
}

inline constexpr bool trustedTimeInputSane(int64_t epoch, int32_t timezoneOffset) {
  return epoch >= TRUSTED_TIME_MIN_EPOCH && epoch <= TRUSTED_TIME_MAX_EPOCH &&
         timezoneOffset >= -TRUSTED_TIME_MAX_TZ_OFFSET &&
         timezoneOffset <= TRUSTED_TIME_MAX_TZ_OFFSET;
}

inline bool trustedTimeAcceptHost(
  TrustedTimeState* state,
  int64_t epoch,
  int32_t timezoneOffset,
  bool authenticated,
  uint32_t now
) {
  if (!state || !authenticated || !trustedTimeInputSane(epoch, timezoneOffset)) return false;
  *state = {TRUSTED_TIME_SECURE_HOST, epoch, now};
  return true;
}

inline bool trustedTimeAcceptSntp(
  TrustedTimeState* state,
  int64_t epoch,
  bool completed,
  uint32_t now
) {
  if (!state || !completed || !trustedTimeInputSane(epoch, 0)) return false;
  *state = {TRUSTED_TIME_SNTP, epoch, now};
  return true;
}

inline constexpr bool trustedTimeFresh(const TrustedTimeState& state, uint32_t now) {
  return state.source != TRUSTED_TIME_NONE &&
         (uint32_t)(now - state.syncAtMs) <= TRUSTED_TIME_MAX_AGE_MS;
}
