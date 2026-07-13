#pragma once

#include <Arduino.h>
#include "ota_manifest_logic.h"
#include "ota_update_logic.h"

struct OtaUpdateRuntimeInputs {
  bool bleConnected;
  bool wifiProvisioned;
  bool wifiOnline;
  bool trustedTime;
  bool prompt;
  bool transfer;
  bool provisioning;
  bool passkey;
  bool functional;
  bool externalPower;
  bool batteryKnown;
  uint8_t batteryPercent;
};

struct OtaUpdateView {
  bool visible;
  bool authenticated;
  bool bootCommitted;
  bool terminal;
  bool error;
  uint8_t percent;
  OtaUpdatePhase phase;
  OtaUpdateFailure failure;
  char version[OTA_VERSION_MAX_BYTES];
  char status[24];
};

void otaUpdatePoll(OtaOfferState* offer, const OtaUpdateRuntimeInputs& inputs);
void otaUpdateConfirm();
bool otaUpdateCancel();
bool otaUpdateActive();
bool otaUpdateTransferStarted();
OtaUpdateView otaUpdateView();
