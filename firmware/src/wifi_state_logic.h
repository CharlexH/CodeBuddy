#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum WifiPhase : uint8_t {
  WIFI_UNPROVISIONED,
  WIFI_PROVISIONING,
  WIFI_CONNECTING,
  WIFI_ONLINE,
  WIFI_ERROR,
};

struct WifiRuntimeState {
  WifiPhase phase;
  bool provisioned;
  uint8_t retryAttempt;
  uint32_t provisioningDeadlineMs;
  uint32_t retryAtMs;
};

static constexpr uint32_t WIFI_PROVISIONING_MAX_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t WIFI_HOST_UTC_FRESH_MS = 15UL * 60UL * 1000UL;

inline constexpr WifiRuntimeState wifiInitialState(bool provisioned) {
  return {provisioned ? WIFI_CONNECTING : WIFI_UNPROVISIONED,
          provisioned, 0, 0, 0};
}

inline WifiRuntimeState wifiPhysicalStart(
  WifiRuntimeState state,
  uint32_t now
) {
  state.phase = WIFI_PROVISIONING;
  state.provisioningDeadlineMs = now + WIFI_PROVISIONING_MAX_MS;
  state.retryAtMs = 0;
  return state;
}

inline constexpr bool wifiProvisioningTimedOut(
  const WifiRuntimeState& state,
  uint32_t now
) {
  return state.provisioningDeadlineMs != 0 &&
         (state.phase == WIFI_PROVISIONING || state.phase == WIFI_CONNECTING ||
          state.phase == WIFI_ERROR) &&
         (int32_t)(now - state.provisioningDeadlineMs) >= 0;
}

inline WifiRuntimeState wifiProvisioningCancel(WifiRuntimeState state) {
  state.phase = state.provisioned ? WIFI_CONNECTING : WIFI_UNPROVISIONED;
  state.provisioningDeadlineMs = 0;
  return state;
}

inline WifiRuntimeState wifiProvisioningStartFailed(WifiRuntimeState state) {
  state.phase = state.provisioned ? WIFI_CONNECTING : WIFI_UNPROVISIONED;
  state.provisioningDeadlineMs = 0;
  state.retryAtMs = 0;
  return state;
}

inline uint32_t wifiRetryDelayMs(uint8_t attempt, uint32_t entropy) {
  uint8_t shift = attempt > 5 ? 5 : attempt;
  uint32_t base = 1000UL << shift;
  if (base > 59750UL) base = 59750UL;
  return base + (entropy % 251UL);
}

inline WifiRuntimeState wifiConnectSucceeded(WifiRuntimeState state) {
  state.phase = WIFI_ONLINE;
  state.provisioned = true;
  state.retryAttempt = 0;
  state.provisioningDeadlineMs = 0;
  state.retryAtMs = 0;
  return state;
}

inline WifiRuntimeState wifiConnectFailed(
  WifiRuntimeState state,
  uint32_t now,
  uint32_t entropy
) {
  state.phase = WIFI_ERROR;
  state.retryAtMs = now + wifiRetryDelayMs(state.retryAttempt, entropy);
  if (state.retryAttempt < 10) state.retryAttempt++;
  return state;
}

inline constexpr bool wifiReconnectWorkDue(
  const WifiRuntimeState& state,
  uint32_t now,
  bool approvalPromptVisible
) {
  return !approvalPromptVisible && state.provisioned &&
    state.phase == WIFI_ERROR && (int32_t)(now - state.retryAtMs) >= 0;
}

inline WifiRuntimeState wifiRetryConnecting(WifiRuntimeState state) {
  state.phase = WIFI_CONNECTING;
  state.retryAtMs = 0;
  return state;
}

inline WifiRuntimeState wifiForget(WifiRuntimeState state) {
  (void)state;
  return wifiInitialState(false);
}

inline bool wifiSsidValid(const char* ssid) {
  if (!ssid) return false;
  size_t len = strnlen(ssid, 33);
  if (len < 1 || len > 32) return false;
  for (size_t i = 0; i < len; ++i) {
    if ((uint8_t)ssid[i] < 0x20) return false;
  }
  return true;
}

inline bool wifiPasswordValid(const char* password) {
  if (!password) return false;
  size_t len = strnlen(password, 64);
  if (len == 0) return true;
  if (len < 8 || len > 63) return false;
  for (size_t i = 0; i < len; ++i) {
    if ((uint8_t)password[i] < 0x20 || (uint8_t)password[i] > 0x7e) return false;
  }
  return true;
}

inline void wifiHtmlEscape(const char* input, char* output, size_t outputSize) {
  if (!output || outputSize == 0) return;
  if (!input) input = "";
  size_t used = 0;
  output[0] = 0;
  for (size_t i = 0; input[i]; ++i) {
    const char* replacement = nullptr;
    switch (input[i]) {
      case '&': replacement = "&amp;"; break;
      case '<': replacement = "&lt;"; break;
      case '>': replacement = "&gt;"; break;
      case '"': replacement = "&quot;"; break;
      case '\'': replacement = "&#39;"; break;
      default: break;
    }
    if (replacement) {
      size_t n = strlen(replacement);
      if (used + n >= outputSize) break;
      memcpy(output + used, replacement, n);
      used += n;
    } else {
      if (used + 1 >= outputSize) break;
      output[used++] = (uint8_t)input[i] < 0x20 ? '?' : input[i];
    }
  }
  output[used] = 0;
}

inline void wifiStatusClip(const char* input, char* output, size_t outputSize) {
  if (!output || outputSize == 0) return;
  if (!input) input = "";
  size_t n = strnlen(input, outputSize - 1);
  for (size_t i = 0; i < n; ++i) {
    output[i] = (uint8_t)input[i] < 0x20 ? '?' : input[i];
  }
  output[n] = 0;
}
