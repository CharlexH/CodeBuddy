#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ota_manifest_logic.h"

constexpr uint32_t OTA_UPDATE_CHUNK_BYTES = 4096;
constexpr uint32_t OTA_UPDATE_SMALL_CHUNK_BYTES = 256;
constexpr uint32_t OTA_UPDATE_READY_TIMEOUT_MS = 30000;
constexpr uint32_t OTA_UPDATE_IO_IDLE_TIMEOUT_MS = 5000;
constexpr uint32_t OTA_UPDATE_RESOURCE_TIMEOUT_MS = 30000;
constexpr uint32_t OTA_UPDATE_FIRMWARE_TIMEOUT_MS = 300000;
constexpr uint32_t OTA_UPDATE_RESTART_RETRY_MS = 100;
constexpr uint8_t OTA_UPDATE_RESTART_API_ATTEMPTS = 3;

enum OtaUpdateGate : uint8_t {
  OTA_GATE_READY = 0,
  OTA_GATE_NO_OFFER,
  OTA_GATE_DISCONNECTED,
  OTA_GATE_WIFI,
  OTA_GATE_TIME,
  OTA_GATE_STALE,
  OTA_GATE_POWER,
  OTA_GATE_CONFLICT,
};

struct OtaUpdateInputs {
  uint32_t nowMs;
  bool offerPending;
  bool bleConnected;
  bool wifiProvisioned;
  bool wifiOnline;
  bool trustedTime;
  bool offerFresh;
  bool externalPower;
  bool batteryKnown;
  uint8_t batteryPercent;
  bool prompt;
  bool transfer;
  bool provisioning;
  bool passkey;
  bool functional;
};

inline bool otaDeadlineExpired(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

inline OtaUpdateGate otaUpdateGate(
  const OtaUpdateInputs& in,
  bool requireOfferCoordination
) {
  if (requireOfferCoordination) {
    if (!in.offerPending) return OTA_GATE_NO_OFFER;
    if (!in.bleConnected) return OTA_GATE_DISCONNECTED;
  }
  if (in.prompt || in.transfer || in.provisioning || in.passkey || in.functional)
    return OTA_GATE_CONFLICT;
  if (!in.wifiProvisioned || !in.wifiOnline) return OTA_GATE_WIFI;
  if (!in.trustedTime) return OTA_GATE_TIME;
  if (requireOfferCoordination && !in.offerFresh) return OTA_GATE_STALE;
  if (!in.externalPower &&
      (!in.batteryKnown || in.batteryPercent < OTA_MIN_BATTERY_PERCENT ||
       in.batteryPercent > 100))
    return OTA_GATE_POWER;
  return OTA_GATE_READY;
}

enum OtaPartitionType : uint8_t {
  OTA_PARTITION_APP = 0,
  OTA_PARTITION_DATA = 1,
};

constexpr uint8_t OTA_PARTITION_SUBTYPE_APP_OTA_MIN_VALUE = 0x10;
constexpr uint8_t OTA_PARTITION_SUBTYPE_APP_OTA_MAX_EXCLUSIVE = 0x20;

struct OtaPartitionInfo {
  uintptr_t identity;
  uint32_t address;
  uint32_t size;
  OtaPartitionType type;
  uint8_t subtype;
  bool pendingVerify;
};

inline bool otaPartitionSubtypeIsOta(uint8_t subtype) {
  return subtype >= OTA_PARTITION_SUBTYPE_APP_OTA_MIN_VALUE &&
    subtype < OTA_PARTITION_SUBTYPE_APP_OTA_MAX_EXCLUSIVE;
}

inline bool otaTargetValid(
  const OtaPartitionInfo* running,
  const OtaPartitionInfo* target,
  uint32_t signedSize
) {
  return running && target && running->identity != target->identity &&
    running->address != target->address && target->type == OTA_PARTITION_APP &&
    otaPartitionSubtypeIsOta(target->subtype) && signedSize > 0 &&
    signedSize <= OTA_SLOT_CAPACITY_BYTES && signedSize <= target->size &&
    !running->pendingVerify;
}

enum OtaPartitionStateQuery : uint8_t {
  OTA_STATE_QUERY_OK = 0,
  OTA_STATE_QUERY_NOT_FOUND,
  OTA_STATE_QUERY_ERROR,
};

enum OtaRunningImageState : uint8_t {
  OTA_RUNNING_IMAGE_VALID = 0,
  OTA_RUNNING_IMAGE_PENDING_VERIFY,
  OTA_RUNNING_IMAGE_OTHER,
};

inline bool otaRunningStateAllowsUpdate(
  OtaPartitionStateQuery query,
  OtaRunningImageState state,
  bool initialLayoutVerified
) {
  if (query == OTA_STATE_QUERY_OK)
    return state == OTA_RUNNING_IMAGE_VALID;
  if (query == OTA_STATE_QUERY_NOT_FOUND) return initialLayoutVerified;
  return false;
}

struct OtaHttpMeta {
  int status;
  int64_t contentLength;
  bool redirected;
  bool identityEncoding;
};

inline bool otaHttpMetaCommonValid(const OtaHttpMeta& meta) {
  return meta.status == 200 && !meta.redirected && meta.identityEncoding &&
    meta.contentLength > 0;
}

inline bool otaHttpMetaBoundedValid(
  const OtaHttpMeta& meta,
  uint32_t maximumLength
) {
  return otaHttpMetaCommonValid(meta) && maximumLength > 0 &&
    static_cast<uint64_t>(meta.contentLength) <= maximumLength;
}

inline bool otaHttpMetaExactValid(
  const OtaHttpMeta& meta,
  uint32_t expectedLength,
  uint32_t maximumLength
) {
  return otaHttpMetaCommonValid(meta) && expectedLength > 0 &&
    expectedLength <= maximumLength &&
    static_cast<uint64_t>(meta.contentLength) == expectedLength;
}

enum OtaHttpBodyDecision : uint8_t {
  OTA_HTTP_BODY_WAIT = 0,
  OTA_HTTP_BODY_READ,
  OTA_HTTP_BODY_COMPLETE,
  OTA_HTTP_BODY_FAILURE,
};

struct OtaHttpBodyState {
  uint32_t expectedBytes;
  uint32_t receivedBytes;
  uint32_t idleDeadlineMs;
  uint32_t absoluteDeadlineMs;
};

struct OtaHttpBodyPoll {
  OtaHttpBodyDecision decision;
  uint32_t bytes;
};

inline OtaHttpBodyState otaHttpBodyState(
  uint32_t now,
  uint32_t expectedBytes,
  uint32_t absoluteTimeoutMs
) {
  return {
    expectedBytes,
    0,
    now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS,
    now + absoluteTimeoutMs,
  };
}

inline OtaHttpBodyPoll otaHttpBodyPoll(
  const OtaHttpBodyState& state,
  uint32_t now,
  uint32_t availableBytes,
  bool connected,
  uint32_t maximumReadBytes
) {
  if (!state.expectedBytes || state.receivedBytes > state.expectedBytes ||
      !maximumReadBytes || otaDeadlineExpired(now, state.absoluteDeadlineMs))
    return {OTA_HTTP_BODY_FAILURE, 0};
  uint32_t remaining = state.expectedBytes - state.receivedBytes;
  if (!remaining)
    return availableBytes == 0
      ? OtaHttpBodyPoll{OTA_HTTP_BODY_COMPLETE, 0}
      : OtaHttpBodyPoll{OTA_HTTP_BODY_FAILURE, 0};
  if (!availableBytes) {
    if (!connected || otaDeadlineExpired(now, state.idleDeadlineMs))
      return {OTA_HTTP_BODY_FAILURE, 0};
    return {OTA_HTTP_BODY_WAIT, 0};
  }
  if (availableBytes > remaining)
    return {OTA_HTTP_BODY_FAILURE, 0};
  uint32_t amount = availableBytes < maximumReadBytes
    ? availableBytes : maximumReadBytes;
  if (amount > remaining) amount = remaining;
  return {OTA_HTTP_BODY_READ, amount};
}

inline bool otaHttpBodyCommit(
  OtaHttpBodyState* state,
  uint32_t now,
  uint32_t bytes
) {
  if (!state || !bytes || state->receivedBytes > state->expectedBytes ||
      bytes > state->expectedBytes - state->receivedBytes)
    return false;
  state->receivedBytes += bytes;
  state->idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
  return true;
}

inline bool otaConstantTimeEqual(
  const uint8_t* left,
  const uint8_t* right,
  size_t size
) {
  if (!left || !right) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

inline int8_t otaHexNibble(char value) {
  if (value >= '0' && value <= '9') return static_cast<int8_t>(value - '0');
  if (value >= 'a' && value <= 'f')
    return static_cast<int8_t>(value - 'a' + 10);
  return -1;
}

inline bool otaDecodeSha256(const char* value, uint8_t output[32]) {
  if (!value || !output) return false;
  for (size_t i = 0; i < 32; ++i) {
    int8_t high = otaHexNibble(value[i * 2]);
    int8_t low = otaHexNibble(value[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return value[64] == 0;
}

inline uint8_t otaProgressPercent(uint64_t complete, uint64_t total) {
  if (!total) return 0;
  if (complete >= total) return 100;
  return static_cast<uint8_t>((complete * UINT64_C(100)) / total);
}

enum OtaUiSurface : uint8_t {
  OTA_UI_HOME = 0,
  OTA_UI_UPDATE,
  OTA_UI_APPROVAL,
};

inline OtaUiSurface otaUiSurface(bool approval, bool update) {
  return approval ? OTA_UI_APPROVAL : update ? OTA_UI_UPDATE : OTA_UI_HOME;
}

enum OtaUpdateEvent : uint8_t {
  OTA_EVENT_OPEN_MANIFEST = 0,
  OTA_EVENT_READ_MANIFEST,
  OTA_EVENT_OPEN_SIGNATURE,
  OTA_EVENT_READ_SIGNATURE,
  OTA_EVENT_AUTHENTICATE,
  OTA_EVENT_SELECT,
  OTA_EVENT_OPEN_FIRMWARE,
  OTA_EVENT_BEGIN,
  OTA_EVENT_DOWNLOAD,
  OTA_EVENT_WRITE,
  OTA_EVENT_FINISH_DOWNLOAD,
  OTA_EVENT_ABORT,
  OTA_EVENT_END,
  OTA_EVENT_READBACK,
  OTA_EVENT_DIGEST_MATCH,
  OTA_EVENT_FINAL_GATE,
  OTA_EVENT_SET_BOOT,
  OTA_EVENT_RESTART,
  OTA_EVENT_RESTART_FALLBACK,
};

enum OtaUpdatePhase : uint8_t {
  OTA_PHASE_CONFIRM = 0,
  OTA_PHASE_WAIT_READY,
  OTA_PHASE_OPEN_MANIFEST,
  OTA_PHASE_READ_MANIFEST,
  OTA_PHASE_OPEN_SIGNATURE,
  OTA_PHASE_READ_SIGNATURE,
  OTA_PHASE_AUTHENTICATE,
  OTA_PHASE_SELECT,
  OTA_PHASE_OPEN_FIRMWARE,
  OTA_PHASE_BEGIN,
  OTA_PHASE_DOWNLOAD,
  OTA_PHASE_WRITE,
  OTA_PHASE_FINISH_DOWNLOAD,
  OTA_PHASE_END,
  OTA_PHASE_READBACK,
  OTA_PHASE_DIGEST,
  OTA_PHASE_FINAL_GATE,
  OTA_PHASE_SET_BOOT,
  OTA_PHASE_RESTART,
  OTA_PHASE_RESTARTING,
  OTA_PHASE_CANCELLED,
  OTA_PHASE_ERROR,
};

inline uint8_t otaUpdateOverallProgress(
  OtaUpdatePhase phase,
  uint64_t receivedBytes,
  uint64_t readbackBytes,
  uint64_t totalBytes
) {
  if (!totalBytes) return 0;
  if (phase >= OTA_PHASE_DIGEST && phase <= OTA_PHASE_RESTARTING) return 100;
  if (phase >= OTA_PHASE_READBACK) {
    uint8_t readback = otaProgressPercent(readbackBytes, totalBytes);
    return static_cast<uint8_t>(80 + (static_cast<uint16_t>(readback) * 20) / 100);
  }
  if (phase >= OTA_PHASE_DOWNLOAD) {
    uint8_t download = otaProgressPercent(receivedBytes, totalBytes);
    return static_cast<uint8_t>((static_cast<uint16_t>(download) * 80) / 100);
  }
  return 0;
}

enum OtaUpdateActionStatus : uint8_t {
  OTA_ACTION_FAILURE = 0,
  OTA_ACTION_PENDING,
  OTA_ACTION_PROGRESS,
  OTA_ACTION_COMPLETE,
};

struct OtaUpdateActionResult {
  OtaUpdateActionStatus status;
  uint32_t bytes;
};

inline OtaUpdateActionResult otaActionFailure() {
  return {OTA_ACTION_FAILURE, 0};
}

inline OtaUpdateActionResult otaActionPending() {
  return {OTA_ACTION_PENDING, 0};
}

inline OtaUpdateActionResult otaActionProgress(uint32_t bytes) {
  return {OTA_ACTION_PROGRESS, bytes};
}

inline OtaUpdateActionResult otaActionComplete(uint32_t bytes = 0) {
  return {OTA_ACTION_COMPLETE, bytes};
}

using OtaUpdateAction = OtaUpdateActionResult (*)(
  void*, OtaUpdateEvent, uint64_t, uint32_t
);

struct OtaUpdateMachine {
  OtaUpdatePhase phase;
  OtaUpdateAction action;
  void* actionContext;
  uint64_t imageSize;
  uint32_t chunkSize;
  uint64_t receivedBytes;
  uint64_t readbackBytes;
  uint32_t pendingChunkBytes;
  bool pendingChunkFinal;
  bool handleValid;
  bool endAttempted;
  bool authenticated;
  bool bootCommitted;
  uint8_t restartAttempts;
  uint32_t nextRestartAttemptMs;
  uint32_t readyDeadlineMs;
};

inline OtaUpdateMachine otaUpdateMachineInitial() {
  OtaUpdateMachine machine = {};
  machine.phase = OTA_PHASE_CONFIRM;
  machine.chunkSize = OTA_UPDATE_CHUNK_BYTES;
  return machine;
}

inline bool otaUpdateTerminal(const OtaUpdateMachine& machine) {
  if (machine.bootCommitted) return false;
  return machine.phase == OTA_PHASE_RESTARTING ||
    machine.phase == OTA_PHASE_CANCELLED || machine.phase == OTA_PHASE_ERROR;
}

inline OtaUpdateActionResult otaUpdateAct(
  OtaUpdateMachine* machine,
  OtaUpdateEvent event,
  uint64_t offset = 0,
  uint32_t maximumBytes = 0
) {
  if (!machine || !machine->action) return otaActionFailure();
  return machine->action(
    machine->actionContext, event, offset, maximumBytes
  );
}

inline bool otaUpdateAtomicSucceeded(const OtaUpdateActionResult& result) {
  return result.status == OTA_ACTION_COMPLETE && result.bytes == 0;
}

inline void otaUpdateAbortHandle(OtaUpdateMachine* machine) {
  if (!machine || !machine->handleValid || machine->endAttempted) return;
  machine->handleValid = false;
  otaUpdateAct(machine, OTA_EVENT_ABORT);
}

inline void otaUpdateFail(OtaUpdateMachine* machine) {
  if (!machine || machine->bootCommitted) return;
  otaUpdateAbortHandle(machine);
  machine->phase = OTA_PHASE_ERROR;
}

inline void otaUpdateCancelMachine(OtaUpdateMachine* machine) {
  if (!machine || machine->bootCommitted) return;
  otaUpdateAbortHandle(machine);
  machine->phase = OTA_PHASE_CANCELLED;
}

inline void otaUpdateScrubMachine(OtaUpdateMachine* machine) {
  if (!machine) return;
  OtaUpdatePhase phase = machine->phase;
  bool bootCommitted = machine->bootCommitted;
  uint8_t restartAttempts = machine->restartAttempts;
  uint32_t nextRestartAttemptMs = machine->nextRestartAttemptMs;
  *machine = {};
  machine->phase = phase;
  machine->bootCommitted = bootCommitted;
  machine->restartAttempts = restartAttempts;
  machine->nextRestartAttemptMs = nextRestartAttemptMs;
}

inline bool otaUpdateReadResultValid(
  const OtaUpdateActionResult& result,
  uint32_t maximumBytes
) {
  return (result.status == OTA_ACTION_PROGRESS ||
          result.status == OTA_ACTION_COMPLETE) &&
    result.bytes > 0 && result.bytes <= maximumBytes;
}

inline void otaUpdateStep(
  OtaUpdateMachine* machine,
  const OtaUpdateInputs& inputs,
  bool physicalConfirm,
  bool physicalCancel
) {
  if (!machine || otaUpdateTerminal(*machine)) return;
  if (machine->bootCommitted) {
    machine->phase = OTA_PHASE_RESTART;
    if (!otaDeadlineExpired(inputs.nowMs, machine->nextRestartAttemptMs)) return;
    OtaUpdateEvent event = machine->restartAttempts <
        OTA_UPDATE_RESTART_API_ATTEMPTS
      ? OTA_EVENT_RESTART : OTA_EVENT_RESTART_FALLBACK;
    otaUpdateAct(machine, event);
    if (machine->restartAttempts < UINT8_MAX) ++machine->restartAttempts;
    machine->nextRestartAttemptMs = inputs.nowMs + OTA_UPDATE_RESTART_RETRY_MS;
    return;
  }
  if (physicalCancel) {
    otaUpdateCancelMachine(machine);
    return;
  }

  if (machine->phase == OTA_PHASE_CONFIRM) {
    OtaUpdateGate gate = otaUpdateGate(inputs, true);
    if (gate == OTA_GATE_CONFLICT) {
      otaUpdateCancelMachine(machine);
      return;
    }
    if (gate != OTA_GATE_READY && gate != OTA_GATE_WIFI && gate != OTA_GATE_TIME) {
      otaUpdateFail(machine);
      return;
    }
    if (!physicalConfirm) return;
    machine->readyDeadlineMs = inputs.nowMs + OTA_UPDATE_READY_TIMEOUT_MS;
    machine->phase = gate == OTA_GATE_READY
      ? OTA_PHASE_OPEN_MANIFEST : OTA_PHASE_WAIT_READY;
    return;
  }

  bool requireCoordination = !machine->authenticated;
  OtaUpdateGate gate = otaUpdateGate(inputs, requireCoordination);
  if (machine->phase == OTA_PHASE_WAIT_READY) {
    if (gate == OTA_GATE_READY) {
      machine->phase = OTA_PHASE_OPEN_MANIFEST;
    } else if (gate == OTA_GATE_CONFLICT) {
      otaUpdateCancelMachine(machine);
    } else if ((gate != OTA_GATE_WIFI && gate != OTA_GATE_TIME) ||
               otaDeadlineExpired(inputs.nowMs, machine->readyDeadlineMs)) {
      otaUpdateFail(machine);
    }
    return;
  }
  if (gate == OTA_GATE_CONFLICT) {
    otaUpdateCancelMachine(machine);
    return;
  }
  if (gate != OTA_GATE_READY) {
    otaUpdateFail(machine);
    return;
  }

  OtaUpdateActionResult result = otaActionFailure();
  switch (machine->phase) {
    case OTA_PHASE_OPEN_MANIFEST:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_MANIFEST);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_READ_MANIFEST;
      break;
    case OTA_PHASE_READ_MANIFEST:
      result = otaUpdateAct(
        machine, OTA_EVENT_READ_MANIFEST, 0, OTA_UPDATE_SMALL_CHUNK_BYTES
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateReadResultValid(result, OTA_UPDATE_SMALL_CHUNK_BYTES))
        return otaUpdateFail(machine);
      if (result.status == OTA_ACTION_COMPLETE)
        machine->phase = OTA_PHASE_OPEN_SIGNATURE;
      break;
    case OTA_PHASE_OPEN_SIGNATURE:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_SIGNATURE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_READ_SIGNATURE;
      break;
    case OTA_PHASE_READ_SIGNATURE:
      result = otaUpdateAct(
        machine, OTA_EVENT_READ_SIGNATURE, 0, OTA_UPDATE_SMALL_CHUNK_BYTES
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateReadResultValid(result, OTA_UPDATE_SMALL_CHUNK_BYTES))
        return otaUpdateFail(machine);
      if (result.status == OTA_ACTION_COMPLETE)
        machine->phase = OTA_PHASE_AUTHENTICATE;
      break;
    case OTA_PHASE_AUTHENTICATE:
      result = otaUpdateAct(machine, OTA_EVENT_AUTHENTICATE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->authenticated = true;
      machine->phase = OTA_PHASE_SELECT;
      break;
    case OTA_PHASE_SELECT:
      result = otaUpdateAct(machine, OTA_EVENT_SELECT);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_OPEN_FIRMWARE;
      break;
    case OTA_PHASE_OPEN_FIRMWARE:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_FIRMWARE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_BEGIN;
      break;
    case OTA_PHASE_BEGIN:
      result = otaUpdateAct(
        machine, OTA_EVENT_BEGIN, 0, static_cast<uint32_t>(machine->imageSize)
      );
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->handleValid = true;
      machine->phase = OTA_PHASE_DOWNLOAD;
      break;
    case OTA_PHASE_DOWNLOAD: {
      uint64_t remaining = machine->imageSize - machine->receivedBytes;
      uint32_t amount = remaining < machine->chunkSize
        ? static_cast<uint32_t>(remaining) : machine->chunkSize;
      if (!amount) return otaUpdateFail(machine);
      result = otaUpdateAct(
        machine, OTA_EVENT_DOWNLOAD, machine->receivedBytes, amount
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateReadResultValid(result, amount)) return otaUpdateFail(machine);
      uint64_t next = machine->receivedBytes + result.bytes;
      bool final = next == machine->imageSize;
      if ((result.status == OTA_ACTION_COMPLETE) != final)
        return otaUpdateFail(machine);
      machine->pendingChunkBytes = result.bytes;
      machine->pendingChunkFinal = final;
      machine->phase = OTA_PHASE_WRITE;
      break;
    }
    case OTA_PHASE_WRITE:
      if (!machine->pendingChunkBytes) return otaUpdateFail(machine);
      result = otaUpdateAct(
        machine, OTA_EVENT_WRITE, machine->receivedBytes,
        machine->pendingChunkBytes
      );
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->receivedBytes += machine->pendingChunkBytes;
      machine->pendingChunkBytes = 0;
      machine->phase = machine->pendingChunkFinal
        ? OTA_PHASE_FINISH_DOWNLOAD : OTA_PHASE_DOWNLOAD;
      machine->pendingChunkFinal = false;
      break;
    case OTA_PHASE_FINISH_DOWNLOAD:
      result = otaUpdateAct(machine, OTA_EVENT_FINISH_DOWNLOAD);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_END;
      break;
    case OTA_PHASE_END:
      machine->endAttempted = true;
      machine->handleValid = false;
      result = otaUpdateAct(machine, OTA_EVENT_END);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_READBACK;
      break;
    case OTA_PHASE_READBACK: {
      uint64_t remaining = machine->imageSize - machine->readbackBytes;
      uint32_t amount = remaining < machine->chunkSize
        ? static_cast<uint32_t>(remaining) : machine->chunkSize;
      if (!amount) return otaUpdateFail(machine);
      result = otaUpdateAct(
        machine, OTA_EVENT_READBACK, machine->readbackBytes, amount
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (result.status != OTA_ACTION_COMPLETE || result.bytes != amount)
        return otaUpdateFail(machine);
      machine->readbackBytes += amount;
      if (machine->readbackBytes == machine->imageSize)
        machine->phase = OTA_PHASE_DIGEST;
      break;
    }
    case OTA_PHASE_DIGEST:
      result = otaUpdateAct(machine, OTA_EVENT_DIGEST_MATCH);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_FINAL_GATE;
      break;
    case OTA_PHASE_FINAL_GATE:
      result = otaUpdateAct(machine, OTA_EVENT_FINAL_GATE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->phase = OTA_PHASE_SET_BOOT;
      break;
    case OTA_PHASE_SET_BOOT:
      result = otaUpdateAct(machine, OTA_EVENT_SET_BOOT);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine);
      machine->bootCommitted = true;
      machine->restartAttempts = 0;
      machine->nextRestartAttemptMs = inputs.nowMs;
      machine->phase = OTA_PHASE_RESTART;
      break;
    default:
      otaUpdateFail(machine);
      break;
  }
}
