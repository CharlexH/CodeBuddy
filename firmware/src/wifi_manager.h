#pragma once

#include <Arduino.h>
#include "wifi_state_logic.h"

struct WifiManagerSnapshot {
  WifiRuntimeState runtime;
  bool portalActive;
  bool systemTimeTrusted;
  int32_t rssi;
  char ssid[33];
  char ip[16];
  char apSsid[24];
  char apPassword[16];
  char message[48];
};

void wifiManagerBegin(const char* deviceSuffix);
void wifiManagerPoll(bool approvalPromptVisible);
bool wifiManagerStartProvisioning();
void wifiManagerCancelProvisioning();
void wifiManagerForget();
bool wifiManagerAcceptHostUtc(int64_t epochSeconds, int32_t timezoneOffset, bool authenticated);
bool wifiManagerSystemTimeTrusted();
bool wifiManagerOnline();
bool wifiManagerProvisioned();
bool wifiManagerUiActive();
WifiManagerSnapshot wifiManagerSnapshot();
