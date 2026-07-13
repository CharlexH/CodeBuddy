#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ota_manifest_logic.h"

constexpr uint32_t OTA_UPDATE_CHUNK_BYTES = 4096;
constexpr uint32_t OTA_UPDATE_SMALL_CHUNK_BYTES = 256;
constexpr uint32_t OTA_UPDATE_READY_TIMEOUT_MS = 30000;
constexpr uint32_t OTA_UPDATE_IO_IDLE_TIMEOUT_MS = 5000;
constexpr uint32_t OTA_UPDATE_OPEN_TIMEOUT_MS = 5000;
constexpr uint32_t OTA_UPDATE_RESOURCE_TIMEOUT_MS = 30000;
constexpr uint32_t OTA_UPDATE_FIRMWARE_TIMEOUT_MS = 300000;
constexpr size_t OTA_UPDATE_HTTP_HEADER_MAX_BYTES = 1024;
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

inline bool otaHttpIoExpired(
  uint32_t now,
  uint32_t absoluteDeadline,
  uint32_t idleDeadline
) {
  return otaDeadlineExpired(now, absoluteDeadline) ||
    otaDeadlineExpired(now, idleDeadline);
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
  OTA_RUNNING_IMAGE_UNDEFINED,
  OTA_RUNNING_IMAGE_PENDING_VERIFY,
  OTA_RUNNING_IMAGE_OTHER,
};

constexpr uint32_t OTA_IMAGE_STATE_NEW_RAW = 0x0U;
constexpr uint32_t OTA_IMAGE_STATE_PENDING_VERIFY_RAW = 0x1U;
constexpr uint32_t OTA_IMAGE_STATE_VALID_RAW = 0x2U;
constexpr uint32_t OTA_IMAGE_STATE_INVALID_RAW = 0x3U;
constexpr uint32_t OTA_IMAGE_STATE_ABORTED_RAW = 0x4U;
constexpr uint32_t OTA_IMAGE_STATE_UNDEFINED_RAW = 0xffffffffU;

inline uint32_t otaReadLittleEndianU32(const uint8_t* bytes) {
  if (!bytes) return 0;
  return static_cast<uint32_t>(bytes[0]) |
    (static_cast<uint32_t>(bytes[1]) << 8) |
    (static_cast<uint32_t>(bytes[2]) << 16) |
    (static_cast<uint32_t>(bytes[3]) << 24);
}

inline OtaRunningImageState otaRunningImageStateFromRaw(uint32_t state) {
  if (state == OTA_IMAGE_STATE_VALID_RAW) return OTA_RUNNING_IMAGE_VALID;
  if (state == OTA_IMAGE_STATE_UNDEFINED_RAW)
    return OTA_RUNNING_IMAGE_UNDEFINED;
  if (state == OTA_IMAGE_STATE_PENDING_VERIFY_RAW)
    return OTA_RUNNING_IMAGE_PENDING_VERIFY;
  return OTA_RUNNING_IMAGE_OTHER;
}

inline bool otaRunningStateAllowsUpdate(
  OtaPartitionStateQuery query,
  OtaRunningImageState state,
  bool initialLayoutVerified
) {
  if (query == OTA_STATE_QUERY_OK) {
    if (state == OTA_RUNNING_IMAGE_VALID) return true;
    return state == OTA_RUNNING_IMAGE_UNDEFINED && initialLayoutVerified;
  }
  if (query == OTA_STATE_QUERY_NOT_FOUND) return initialLayoutVerified;
  return false;
}

struct OtaHttpMeta {
  int status;
  int64_t contentLength;
  bool redirected;
  bool identityEncoding;
};

inline char otaAsciiLower(char value) {
  return value >= 'A' && value <= 'Z'
    ? static_cast<char>(value - 'A' + 'a') : value;
}

inline bool otaAsciiCaseEqual(
  const uint8_t* value,
  size_t length,
  const char* expected
) {
  if (!value || !expected || strlen(expected) != length) return false;
  for (size_t index = 0; index < length; ++index) {
    if (otaAsciiLower(static_cast<char>(value[index])) !=
        otaAsciiLower(expected[index])) return false;
  }
  return true;
}

inline bool otaParseStrictHttpResponseHeader(
  const uint8_t* header,
  size_t length,
  OtaHttpMeta* output
) {
  if (!header || !output || length < 12 ||
      length > OTA_UPDATE_HTTP_HEADER_MAX_BYTES ||
      memcmp(header + length - 4, "\r\n\r\n", 4) != 0)
    return false;

  size_t lineEnd = 0;
  while (lineEnd + 1 < length &&
         !(header[lineEnd] == '\r' && header[lineEnd + 1] == '\n'))
    ++lineEnd;
  if (lineEnd + 1 >= length || lineEnd < 12 ||
      memcmp(header, "HTTP/1.1 ", 9) != 0 ||
      !otaAsciiDigit(static_cast<char>(header[9])) ||
      !otaAsciiDigit(static_cast<char>(header[10])) ||
      !otaAsciiDigit(static_cast<char>(header[11])) ||
      (lineEnd > 12 && header[12] != ' '))
    return false;
  for (size_t index = 12; index < lineEnd; ++index) {
    if (header[index] < 0x20 || header[index] > 0x7e) return false;
  }

  OtaHttpMeta parsed = {};
  parsed.status = (header[9] - '0') * 100 +
    (header[10] - '0') * 10 + (header[11] - '0');
  parsed.contentLength = -1;
  parsed.identityEncoding = true;
  bool contentLengthSeen = false;
  size_t position = lineEnd + 2;
  while (position + 1 < length) {
    if (header[position] == '\r' && header[position + 1] == '\n') {
      position += 2;
      break;
    }
    lineEnd = position;
    while (lineEnd + 1 < length &&
           !(header[lineEnd] == '\r' && header[lineEnd + 1] == '\n'))
      ++lineEnd;
    if (lineEnd + 1 >= length || lineEnd == position ||
        header[position] == ' ' || header[position] == '\t')
      return false;
    size_t colon = position;
    while (colon < lineEnd && header[colon] != ':') ++colon;
    if (colon == position || colon == lineEnd) return false;
    for (size_t index = position; index < colon; ++index) {
      char value = static_cast<char>(header[index]);
      if (!(otaAsciiAlpha(value) || otaAsciiDigit(value) || value == '-'))
        return false;
    }
    for (size_t index = colon + 1; index < lineEnd; ++index) {
      if (header[index] != '\t' &&
          (header[index] < 0x20 || header[index] > 0x7e))
        return false;
    }
    size_t valueStart = colon + 1;
    while (valueStart < lineEnd &&
           (header[valueStart] == ' ' || header[valueStart] == '\t'))
      ++valueStart;
    size_t valueEnd = lineEnd;
    while (valueEnd > valueStart &&
           (header[valueEnd - 1] == ' ' || header[valueEnd - 1] == '\t'))
      --valueEnd;
    size_t nameLength = colon - position;
    size_t valueLength = valueEnd - valueStart;
    if (otaAsciiCaseEqual(header + position, nameLength, "Content-Length")) {
      uint32_t contentLength = 0;
      if (contentLengthSeen ||
          !otaParseCanonicalUint(
            reinterpret_cast<const char*>(header + valueStart),
            valueLength,
            UINT32_MAX,
            &contentLength
          )) return false;
      contentLengthSeen = true;
      parsed.contentLength = contentLength;
    } else if (otaAsciiCaseEqual(
                 header + position, nameLength, "Transfer-Encoding"
               )) {
      // Any transfer coding is forbidden: the OTA body must be byte-for-byte
      // identical to the signed Content-Length representation.
      parsed.identityEncoding = false;
    } else if (otaAsciiCaseEqual(
                 header + position, nameLength, "Content-Encoding"
               )) {
      if (!otaAsciiCaseEqual(header + valueStart, valueLength, "identity"))
        parsed.identityEncoding = false;
    } else if (otaAsciiCaseEqual(header + position, nameLength, "Location")) {
      parsed.redirected = true;
    }
    position = lineEnd + 2;
  }
  if (position != length) return false;
  *output = parsed;
  return true;
}

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

inline bool otaPrefetchedBodyValid(
  size_t receivedBytes,
  size_t headerBytes,
  uint32_t contentLength
) {
  return headerBytes <= receivedBytes &&
    receivedBytes - headerBytes <= contentLength;
}

enum OtaHttpEofDecision : uint8_t {
  OTA_HTTP_EOF_WAIT = 0,
  OTA_HTTP_EOF_COMPLETE,
  OTA_HTTP_EOF_FAILURE,
};

inline OtaHttpEofDecision otaHttpEofDecision(
  int32_t result,
  int32_t wantRead,
  int32_t wantWrite
) {
  if (result == wantRead || result == wantWrite) return OTA_HTTP_EOF_WAIT;
  return result == 0 ? OTA_HTTP_EOF_COMPLETE : OTA_HTTP_EOF_FAILURE;
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

enum OtaUpdateFailure : uint8_t {
  OTA_UPDATE_FAILURE_NONE = 0,
  OTA_UPDATE_FAILURE_WIFI,
  OTA_UPDATE_FAILURE_TRUST,
  OTA_UPDATE_FAILURE_MANIFEST,
  OTA_UPDATE_FAILURE_VERSION,
  OTA_UPDATE_FAILURE_DOWNLOAD,
  OTA_UPDATE_FAILURE_HASH,
  OTA_UPDATE_FAILURE_POWER,
  OTA_UPDATE_FAILURE_TIMEOUT,
  OTA_UPDATE_FAILURE_ROLLBACK,
  OTA_UPDATE_FAILURE_CONFLICT,
  OTA_UPDATE_FAILURE_RECONNECT,
};

inline OtaUpdateFailure otaUpdateFailureForGate(
  OtaUpdateGate gate,
  bool deadlineExpired
) {
  if (deadlineExpired) return OTA_UPDATE_FAILURE_TIMEOUT;
  switch (gate) {
    case OTA_GATE_WIFI: return OTA_UPDATE_FAILURE_WIFI;
    case OTA_GATE_TIME: return OTA_UPDATE_FAILURE_TRUST;
    case OTA_GATE_POWER: return OTA_UPDATE_FAILURE_POWER;
    case OTA_GATE_CONFLICT: return OTA_UPDATE_FAILURE_CONFLICT;
    case OTA_GATE_NO_OFFER:
    case OTA_GATE_DISCONNECTED:
    case OTA_GATE_STALE: return OTA_UPDATE_FAILURE_RECONNECT;
    case OTA_GATE_READY: return OTA_UPDATE_FAILURE_NONE;
  }
  return OTA_UPDATE_FAILURE_DOWNLOAD;
}

inline OtaUpdateFailure otaUpdateFailureForManifest(OtaManifestResult result) {
  switch (result) {
    case OTA_MANIFEST_OK: return OTA_UPDATE_FAILURE_NONE;
    case OTA_MANIFEST_SIGNATURE_INVALID: return OTA_UPDATE_FAILURE_TRUST;
    case OTA_MANIFEST_VERSION_INVALID:
    case OTA_MANIFEST_NOT_NEWER: return OTA_UPDATE_FAILURE_VERSION;
    default: return OTA_UPDATE_FAILURE_MANIFEST;
  }
}

inline OtaUpdateFailure otaUpdateFailureForEvent(OtaUpdateEvent event) {
  switch (event) {
    case OTA_EVENT_OPEN_MANIFEST:
    case OTA_EVENT_OPEN_SIGNATURE:
    case OTA_EVENT_OPEN_FIRMWARE: return OTA_UPDATE_FAILURE_TRUST;
    case OTA_EVENT_READ_MANIFEST:
    case OTA_EVENT_READ_SIGNATURE: return OTA_UPDATE_FAILURE_MANIFEST;
    case OTA_EVENT_AUTHENTICATE: return OTA_UPDATE_FAILURE_TRUST;
    case OTA_EVENT_DIGEST_MATCH: return OTA_UPDATE_FAILURE_HASH;
    case OTA_EVENT_SELECT:
    case OTA_EVENT_SET_BOOT: return OTA_UPDATE_FAILURE_ROLLBACK;
    case OTA_EVENT_DOWNLOAD:
    case OTA_EVENT_WRITE:
    case OTA_EVENT_FINISH_DOWNLOAD:
    case OTA_EVENT_BEGIN:
    case OTA_EVENT_END:
    case OTA_EVENT_READBACK:
    case OTA_EVENT_FINAL_GATE: return OTA_UPDATE_FAILURE_DOWNLOAD;
    case OTA_EVENT_ABORT:
    case OTA_EVENT_RESTART:
    case OTA_EVENT_RESTART_FALLBACK: return OTA_UPDATE_FAILURE_DOWNLOAD;
  }
  return OTA_UPDATE_FAILURE_DOWNLOAD;
}

inline OtaUpdateFailure otaUpdateFailureForPhase(OtaUpdatePhase phase) {
  switch (phase) {
    case OTA_PHASE_OPEN_MANIFEST: return otaUpdateFailureForEvent(OTA_EVENT_OPEN_MANIFEST);
    case OTA_PHASE_READ_MANIFEST: return otaUpdateFailureForEvent(OTA_EVENT_READ_MANIFEST);
    case OTA_PHASE_OPEN_SIGNATURE: return otaUpdateFailureForEvent(OTA_EVENT_OPEN_SIGNATURE);
    case OTA_PHASE_READ_SIGNATURE: return otaUpdateFailureForEvent(OTA_EVENT_READ_SIGNATURE);
    case OTA_PHASE_AUTHENTICATE: return otaUpdateFailureForEvent(OTA_EVENT_AUTHENTICATE);
    case OTA_PHASE_SELECT: return otaUpdateFailureForEvent(OTA_EVENT_SELECT);
    case OTA_PHASE_OPEN_FIRMWARE: return otaUpdateFailureForEvent(OTA_EVENT_OPEN_FIRMWARE);
    case OTA_PHASE_BEGIN: return otaUpdateFailureForEvent(OTA_EVENT_BEGIN);
    case OTA_PHASE_DOWNLOAD: return otaUpdateFailureForEvent(OTA_EVENT_DOWNLOAD);
    case OTA_PHASE_WRITE: return otaUpdateFailureForEvent(OTA_EVENT_WRITE);
    case OTA_PHASE_FINISH_DOWNLOAD: return otaUpdateFailureForEvent(OTA_EVENT_FINISH_DOWNLOAD);
    case OTA_PHASE_END: return otaUpdateFailureForEvent(OTA_EVENT_END);
    case OTA_PHASE_READBACK: return otaUpdateFailureForEvent(OTA_EVENT_READBACK);
    case OTA_PHASE_DIGEST: return otaUpdateFailureForEvent(OTA_EVENT_DIGEST_MATCH);
    case OTA_PHASE_FINAL_GATE: return otaUpdateFailureForEvent(OTA_EVENT_FINAL_GATE);
    case OTA_PHASE_SET_BOOT: return otaUpdateFailureForEvent(OTA_EVENT_SET_BOOT);
    default: return OTA_UPDATE_FAILURE_DOWNLOAD;
  }
}

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
  OtaUpdateFailure failure;
};

inline OtaUpdateActionResult otaActionFailure(
  OtaUpdateFailure failure = OTA_UPDATE_FAILURE_NONE
) {
  return {OTA_ACTION_FAILURE, 0, failure};
}

inline OtaUpdateActionResult otaActionPending() {
  return {OTA_ACTION_PENDING, 0, OTA_UPDATE_FAILURE_NONE};
}

inline OtaUpdateActionResult otaActionProgress(uint32_t bytes) {
  return {OTA_ACTION_PROGRESS, bytes, OTA_UPDATE_FAILURE_NONE};
}

inline OtaUpdateActionResult otaActionComplete(uint32_t bytes = 0) {
  return {OTA_ACTION_COMPLETE, bytes, OTA_UPDATE_FAILURE_NONE};
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
  OtaUpdateFailure failure;
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

inline bool otaUpdateCancellationAllowed(
  bool active,
  const OtaUpdateMachine& machine
) {
  return active && !machine.bootCommitted && !otaUpdateTerminal(machine) &&
    machine.phase < OTA_PHASE_SET_BOOT;
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

inline void otaUpdateFail(
  OtaUpdateMachine* machine,
  OtaUpdateFailure failure = OTA_UPDATE_FAILURE_NONE
) {
  if (!machine || machine->bootCommitted) return;
  machine->failure = failure == OTA_UPDATE_FAILURE_NONE
    ? otaUpdateFailureForPhase(machine->phase) : failure;
  otaUpdateAbortHandle(machine);
  machine->phase = OTA_PHASE_ERROR;
}

inline void otaUpdateCancelMachine(
  OtaUpdateMachine* machine,
  OtaUpdateFailure failure = OTA_UPDATE_FAILURE_NONE
) {
  if (!machine || machine->bootCommitted) return;
  machine->failure = failure;
  otaUpdateAbortHandle(machine);
  machine->phase = OTA_PHASE_CANCELLED;
}

inline void otaUpdateScrubMachine(OtaUpdateMachine* machine) {
  if (!machine) return;
  OtaUpdatePhase phase = machine->phase;
  bool bootCommitted = machine->bootCommitted;
  uint8_t restartAttempts = machine->restartAttempts;
  uint32_t nextRestartAttemptMs = machine->nextRestartAttemptMs;
  OtaUpdateFailure failure = machine->failure;
  *machine = {};
  machine->phase = phase;
  machine->bootCommitted = bootCommitted;
  machine->restartAttempts = restartAttempts;
  machine->nextRestartAttemptMs = nextRestartAttemptMs;
  machine->failure = failure;
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
  if (physicalCancel && otaUpdateCancellationAllowed(true, *machine)) {
    otaUpdateCancelMachine(machine);
    return;
  }

  if (machine->phase == OTA_PHASE_CONFIRM) {
    OtaUpdateGate gate = otaUpdateGate(inputs, true);
    if (gate == OTA_GATE_CONFLICT) {
      otaUpdateCancelMachine(machine, OTA_UPDATE_FAILURE_CONFLICT);
      return;
    }
    if (gate != OTA_GATE_READY && gate != OTA_GATE_WIFI && gate != OTA_GATE_TIME) {
      otaUpdateFail(machine, otaUpdateFailureForGate(gate, false));
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
      otaUpdateCancelMachine(machine, OTA_UPDATE_FAILURE_CONFLICT);
    } else if ((gate != OTA_GATE_WIFI && gate != OTA_GATE_TIME) ||
               otaDeadlineExpired(inputs.nowMs, machine->readyDeadlineMs)) {
      bool expired = otaDeadlineExpired(inputs.nowMs, machine->readyDeadlineMs);
      otaUpdateFail(machine, otaUpdateFailureForGate(gate, expired));
    }
    return;
  }
  if (gate == OTA_GATE_CONFLICT) {
    otaUpdateCancelMachine(machine, OTA_UPDATE_FAILURE_CONFLICT);
    return;
  }
  if (gate != OTA_GATE_READY) {
    otaUpdateFail(machine, otaUpdateFailureForGate(gate, false));
    return;
  }

  OtaUpdateActionResult result = otaActionFailure();
  switch (machine->phase) {
    case OTA_PHASE_OPEN_MANIFEST:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_MANIFEST);
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_READ_MANIFEST;
      break;
    case OTA_PHASE_READ_MANIFEST:
      result = otaUpdateAct(
        machine, OTA_EVENT_READ_MANIFEST, 0, OTA_UPDATE_SMALL_CHUNK_BYTES
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateReadResultValid(result, OTA_UPDATE_SMALL_CHUNK_BYTES))
        return otaUpdateFail(machine, result.failure);
      if (result.status == OTA_ACTION_COMPLETE)
        machine->phase = OTA_PHASE_OPEN_SIGNATURE;
      break;
    case OTA_PHASE_OPEN_SIGNATURE:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_SIGNATURE);
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_READ_SIGNATURE;
      break;
    case OTA_PHASE_READ_SIGNATURE:
      result = otaUpdateAct(
        machine, OTA_EVENT_READ_SIGNATURE, 0, OTA_UPDATE_SMALL_CHUNK_BYTES
      );
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateReadResultValid(result, OTA_UPDATE_SMALL_CHUNK_BYTES))
        return otaUpdateFail(machine, result.failure);
      if (result.status == OTA_ACTION_COMPLETE)
        machine->phase = OTA_PHASE_AUTHENTICATE;
      break;
    case OTA_PHASE_AUTHENTICATE:
      result = otaUpdateAct(machine, OTA_EVENT_AUTHENTICATE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->authenticated = true;
      machine->phase = OTA_PHASE_SELECT;
      break;
    case OTA_PHASE_SELECT:
      result = otaUpdateAct(machine, OTA_EVENT_SELECT);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_OPEN_FIRMWARE;
      break;
    case OTA_PHASE_OPEN_FIRMWARE:
      result = otaUpdateAct(machine, OTA_EVENT_OPEN_FIRMWARE);
      if (result.status == OTA_ACTION_PENDING) break;
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_BEGIN;
      break;
    case OTA_PHASE_BEGIN:
      result = otaUpdateAct(
        machine, OTA_EVENT_BEGIN, 0, static_cast<uint32_t>(machine->imageSize)
      );
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
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
      if (!otaUpdateReadResultValid(result, amount))
        return otaUpdateFail(machine, result.failure);
      uint64_t next = machine->receivedBytes + result.bytes;
      bool final = next == machine->imageSize;
      if ((result.status == OTA_ACTION_COMPLETE) != final)
        return otaUpdateFail(machine, result.failure);
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
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->receivedBytes += machine->pendingChunkBytes;
      machine->pendingChunkBytes = 0;
      machine->phase = machine->pendingChunkFinal
        ? OTA_PHASE_FINISH_DOWNLOAD : OTA_PHASE_DOWNLOAD;
      machine->pendingChunkFinal = false;
      break;
    case OTA_PHASE_FINISH_DOWNLOAD:
      result = otaUpdateAct(machine, OTA_EVENT_FINISH_DOWNLOAD);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_END;
      break;
    case OTA_PHASE_END:
      machine->endAttempted = true;
      machine->handleValid = false;
      result = otaUpdateAct(machine, OTA_EVENT_END);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
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
        return otaUpdateFail(machine, result.failure);
      machine->readbackBytes += amount;
      if (machine->readbackBytes == machine->imageSize)
        machine->phase = OTA_PHASE_DIGEST;
      break;
    }
    case OTA_PHASE_DIGEST:
      result = otaUpdateAct(machine, OTA_EVENT_DIGEST_MATCH);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_FINAL_GATE;
      break;
    case OTA_PHASE_FINAL_GATE:
      result = otaUpdateAct(machine, OTA_EVENT_FINAL_GATE);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
      machine->phase = OTA_PHASE_SET_BOOT;
      break;
    case OTA_PHASE_SET_BOOT:
      result = otaUpdateAct(machine, OTA_EVENT_SET_BOOT);
      if (!otaUpdateAtomicSucceeded(result)) return otaUpdateFail(machine, result.failure);
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
