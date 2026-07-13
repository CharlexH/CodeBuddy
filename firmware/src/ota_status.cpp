#include "ota_status.h"

#include <Arduino.h>
#include <string.h>

#include "ble_bridge.h"
#include "firmware_version.h"
#include "ota_boot_health.h"
#include "ota_status_logic.h"

namespace {
char activeNonce[49] = {};
uint32_t activeGeneration = 0;
OtaStatusCadence cadence = otaStatusCadenceInitial();

void writeStatus(
  const char* nonce,
  uint32_t generation,
  OtaStatusPhase phase,
  uint8_t percent,
  const char* error,
  bool cadenceLimited
) {
  if (cadenceLimited &&
      !otaStatusShouldEmit(&cadence, phase, percent, error)) return;
  char payload[256] = {};
  size_t length = otaStatusBuildJson(
    payload, sizeof(payload), nonce, generation, phase, percent,
    CODE_BUDDY_FIRMWARE_VERSION, otaBootHealthStatusLabel(), error
  );
  if (!length) return;
  Serial.write(reinterpret_cast<const uint8_t*>(payload), length);
  bleWrite(reinterpret_cast<const uint8_t*>(payload), length);
}

OtaStatusPhase phaseForView(const OtaUpdateView& view, const char** error) {
  *error = "";
  if (view.phase == OTA_PHASE_CONFIRM) return OTA_STATUS_AWAIT_CONFIRM;
  if (view.phase == OTA_PHASE_WAIT_READY) return OTA_STATUS_ACCEPTED;
  if (view.phase >= OTA_PHASE_OPEN_MANIFEST &&
      view.phase <= OTA_PHASE_AUTHENTICATE) return OTA_STATUS_ACCEPTED;
  if (view.authenticated && view.phase < OTA_PHASE_DOWNLOAD)
    return OTA_STATUS_AUTHENTICATED;
  if (view.phase >= OTA_PHASE_DOWNLOAD && view.phase < OTA_PHASE_READBACK)
    return OTA_STATUS_DOWNLOAD;
  if (view.phase >= OTA_PHASE_READBACK && view.phase < OTA_PHASE_SET_BOOT)
    return OTA_STATUS_READBACK;
  if (view.phase == OTA_PHASE_SET_BOOT || view.phase == OTA_PHASE_RESTART)
    return OTA_STATUS_BOOT_COMMITTED;
  if (view.phase == OTA_PHASE_RESTARTING) return OTA_STATUS_RESTARTING;
  if (view.phase == OTA_PHASE_CANCELLED) {
    *error = "cancelled";
    return OTA_STATUS_CANCELLED;
  }
  if (view.phase == OTA_PHASE_ERROR) {
    *error = "download";
    return OTA_STATUS_ERROR;
  }
  return OTA_STATUS_ACCEPTED;
}
}  // namespace

void otaStatusBindOffer(const OtaOfferState& offer) {
  if (!offer.pending || !otaStatusNonceValid(offer.nonce) || !offer.generation)
    return;
  memcpy(activeNonce, offer.nonce, sizeof(activeNonce));
  activeGeneration = offer.generation;
  cadence = otaStatusCadenceInitial();
  writeStatus(
    activeNonce, activeGeneration, OTA_STATUS_OFFER_RECEIVED, 0, "", true
  );
  writeStatus(
    activeNonce, activeGeneration, OTA_STATUS_AWAIT_CONFIRM, 0, "", true
  );
}

void otaStatusReject(const char* nonce, uint32_t generation, const char* error) {
  const char* safe = otaStatusErrorValid(error) ? error : "rejected";
  writeStatus(
    nonce, generation,
    strcmp(safe, "busy") == 0 ? OTA_STATUS_BUSY : OTA_STATUS_REJECTED,
    0, safe, false
  );
}

bool otaStatusReplyRunning(const char* nonce, uint32_t generation) {
  if (!otaStatusNonceValid(nonce) || !generation) return false;
  OtaStatusPhase phase = otaBootHealthSupervising()
    ? OTA_STATUS_BOOT_HEALTH : OTA_STATUS_RUNNING;
  writeStatus(nonce, generation, phase, phase == OTA_STATUS_RUNNING ? 100 : 0, "", false);
  return true;
}

bool otaStatusCancel(const char* nonce, uint32_t generation) {
  if (!otaStatusNonceValid(nonce) || generation != activeGeneration ||
      strcmp(nonce, activeNonce) != 0) return false;
  otaUpdateCancel();
  writeStatus(activeNonce, activeGeneration, OTA_STATUS_CANCELLED, 0, "cancelled", true);
  return true;
}

void otaStatusPoll(const OtaUpdateView& view) {
  if (!activeGeneration || !activeNonce[0] || !view.visible) return;
  const char* error = "";
  OtaStatusPhase phase = phaseForView(view, &error);
  writeStatus(
    activeNonce, activeGeneration, phase, view.percent, error, true
  );
}
