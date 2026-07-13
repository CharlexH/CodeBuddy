#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_update_logic.h"

enum OtaStatusPhase : uint8_t {
  OTA_STATUS_RUNNING = 0,
  OTA_STATUS_OFFER_RECEIVED,
  OTA_STATUS_AWAIT_CONFIRM,
  OTA_STATUS_ACCEPTED,
  OTA_STATUS_REJECTED,
  OTA_STATUS_BUSY,
  OTA_STATUS_AUTHENTICATED,
  OTA_STATUS_DOWNLOAD,
  OTA_STATUS_READBACK,
  OTA_STATUS_BOOT_COMMITTED,
  OTA_STATUS_RESTARTING,
  OTA_STATUS_BOOT_HEALTH,
  OTA_STATUS_CANCELLED,
  OTA_STATUS_ERROR,
};

struct OtaStatusCadence {
  bool initialized;
  OtaStatusPhase phase;
  uint8_t progressBucket;
  char error[16];
};

inline OtaStatusCadence otaStatusCadenceInitial() {
  OtaStatusCadence state = {};
  return state;
}

inline bool otaStatusNonceValid(const char* nonce) {
  if (!nonce) return false;
  size_t length = strnlen(nonce, 49);
  if (length < 24 || length > 48) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = nonce[i];
    bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') || c == '_' || c == '-';
    if (!safe) return false;
  }
  return true;
}

inline const char* otaStatusPhaseLabel(OtaStatusPhase phase) {
  switch (phase) {
    case OTA_STATUS_RUNNING: return "running";
    case OTA_STATUS_OFFER_RECEIVED: return "offer-received";
    case OTA_STATUS_AWAIT_CONFIRM: return "await-confirm";
    case OTA_STATUS_ACCEPTED: return "accepted";
    case OTA_STATUS_REJECTED: return "rejected";
    case OTA_STATUS_BUSY: return "busy";
    case OTA_STATUS_AUTHENTICATED: return "authenticated";
    case OTA_STATUS_DOWNLOAD: return "download";
    case OTA_STATUS_READBACK: return "readback";
    case OTA_STATUS_BOOT_COMMITTED: return "boot-committed";
    case OTA_STATUS_RESTARTING: return "restarting";
    case OTA_STATUS_BOOT_HEALTH: return "boot-health";
    case OTA_STATUS_CANCELLED: return "cancelled";
    case OTA_STATUS_ERROR: return "error";
  }
  return "error";
}

inline bool otaStatusErrorValid(const char* error) {
  if (!error) return false;
  const char* allowed[] = {
    "", "busy", "cancelled", "conflict", "download", "hash",
    "manifest", "power", "reconnect", "rejected", "rollback",
    "timeout", "trust", "version", "wifi",
  };
  for (const char* value : allowed) {
    if (strcmp(error, value) == 0) return true;
  }
  return false;
}

inline const char* otaStatusFailureLabel(OtaUpdateFailure failure) {
  switch (failure) {
    case OTA_UPDATE_FAILURE_WIFI: return "wifi";
    case OTA_UPDATE_FAILURE_TRUST: return "trust";
    case OTA_UPDATE_FAILURE_MANIFEST: return "manifest";
    case OTA_UPDATE_FAILURE_VERSION: return "version";
    case OTA_UPDATE_FAILURE_DOWNLOAD: return "download";
    case OTA_UPDATE_FAILURE_HASH: return "hash";
    case OTA_UPDATE_FAILURE_POWER: return "power";
    case OTA_UPDATE_FAILURE_TIMEOUT: return "timeout";
    case OTA_UPDATE_FAILURE_ROLLBACK: return "rollback";
    case OTA_UPDATE_FAILURE_CONFLICT: return "conflict";
    case OTA_UPDATE_FAILURE_RECONNECT: return "reconnect";
    case OTA_UPDATE_FAILURE_NONE: return "download";
  }
  return "download";
}

struct OtaStatusProjection {
  OtaStatusPhase phase;
  const char* error;
};

inline OtaStatusProjection otaStatusProjectUpdate(
  OtaUpdatePhase phase,
  bool bootCommitted,
  OtaUpdateFailure failure
) {
  if (phase == OTA_PHASE_CONFIRM) return {OTA_STATUS_AWAIT_CONFIRM, ""};
  if (phase == OTA_PHASE_WAIT_READY) return {OTA_STATUS_ACCEPTED, ""};
  if (phase >= OTA_PHASE_OPEN_MANIFEST && phase <= OTA_PHASE_AUTHENTICATE)
    return {OTA_STATUS_ACCEPTED, ""};
  if (phase >= OTA_PHASE_DOWNLOAD && phase < OTA_PHASE_READBACK)
    return {OTA_STATUS_DOWNLOAD, ""};
  if (phase >= OTA_PHASE_READBACK && phase < OTA_PHASE_SET_BOOT)
    return {OTA_STATUS_READBACK, ""};
  if (phase == OTA_PHASE_SET_BOOT && !bootCommitted)
    return {OTA_STATUS_READBACK, ""};
  if (phase == OTA_PHASE_SET_BOOT || phase == OTA_PHASE_RESTART)
    return {OTA_STATUS_BOOT_COMMITTED, ""};
  if (phase == OTA_PHASE_RESTARTING) return {OTA_STATUS_RESTARTING, ""};
  if (phase == OTA_PHASE_CANCELLED) {
    return {
      OTA_STATUS_CANCELLED,
      failure == OTA_UPDATE_FAILURE_NONE
        ? "cancelled" : otaStatusFailureLabel(failure)
    };
  }
  if (phase == OTA_PHASE_ERROR)
    return {OTA_STATUS_ERROR, otaStatusFailureLabel(failure)};
  return {OTA_STATUS_ACCEPTED, ""};
}

inline const char* otaStatusHealthLabel(const char* raw) {
  if (!raw) return "error";
  if (strcmp(raw, "ordinary") == 0) return "ordinary";
  if (strcmp(raw, "pending") == 0 || strcmp(raw, "validating") == 0)
    return "monitoring";
  if (strcmp(raw, "valid") == 0) return "valid";
  if (strcmp(raw, "rollback") == 0) return "rollback";
  return "error";
}

inline bool otaStatusSafeVersion(const char* version) {
  if (!version) return false;
  size_t length = strnlen(version, 64);
  if (!length || length >= 64) return false;
  for (size_t i = 0; i < length; ++i) {
    char c = version[i];
    bool safe = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '+';
    if (!safe) return false;
  }
  return true;
}

inline bool otaStatusShouldEmit(
  OtaStatusCadence* state,
  OtaStatusPhase phase,
  uint8_t percent,
  const char* error
) {
  if (!state || percent > 100 || !otaStatusErrorValid(error)) return false;
  uint8_t bucket = static_cast<uint8_t>(percent / 10);
  bool changed = !state->initialized || state->phase != phase ||
    state->progressBucket != bucket || strcmp(state->error, error) != 0;
  if (!changed) return false;
  state->initialized = true;
  state->phase = phase;
  state->progressBucket = bucket;
  strncpy(state->error, error, sizeof(state->error) - 1);
  state->error[sizeof(state->error) - 1] = 0;
  return true;
}

inline size_t otaStatusBuildJson(
  char* output,
  size_t capacity,
  const char* nonce,
  uint32_t generation,
  OtaStatusPhase phase,
  uint8_t percent,
  const char* version,
  const char* health,
  const char* error,
  bool cancelApplied
) {
  if (!output || !capacity || !otaStatusNonceValid(nonce) || !generation ||
      percent > 100 || !otaStatusSafeVersion(version) ||
      !health || !otaStatusErrorValid(error)) return 0;
  const char* safeHealth = otaStatusHealthLabel(health);
  int written = snprintf(
    output, capacity,
    "{\"cmd\":\"ota_status\",\"nonce\":\"%s\",\"generation\":%lu,"
    "\"phase\":\"%s\",\"percent\":%u,\"version\":\"%s\","
    "\"health\":\"%s\",\"error\":\"%s\","
    "\"cancel_applied\":%s}\n",
    nonce, static_cast<unsigned long>(generation), otaStatusPhaseLabel(phase),
    percent, version, safeHealth, error, cancelApplied ? "true" : "false"
  );
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    output[0] = 0;
    return 0;
  }
  return static_cast<size_t>(written);
}
