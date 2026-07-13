#pragma once

#include <stdint.h>

constexpr uint32_t OTA_BOOT_HEALTH_TIMEOUT_MS = 30000;
constexpr uint32_t OTA_BOOT_ROLLBACK_RESTART_DELAY_MS = 100;
constexpr uint32_t OTA_BOOT_ROLLBACK_RECORD_MAGIC = 0x43425242;  // "CBRB"
constexpr uint8_t OTA_BOOT_ROLLBACK_RECORD_VERSION = 1;

enum OtaBootStateQuery : uint8_t {
  OTA_BOOT_QUERY_NON_PENDING = 0,
  OTA_BOOT_QUERY_PENDING_VERIFY,
  OTA_BOOT_QUERY_UNKNOWN,
};

enum OtaBootReadyBit : uint8_t {
  OTA_BOOT_READY_DISPLAY = 1U << 0,
  OTA_BOOT_READY_STORAGE = 1U << 1,
  OTA_BOOT_READY_BUTTONS = 1U << 2,
  OTA_BOOT_READY_BLE = 1U << 3,
  OTA_BOOT_READY_EVENT_LOOP = 1U << 4,
};

constexpr uint8_t OTA_BOOT_READY_ALL =
  OTA_BOOT_READY_DISPLAY | OTA_BOOT_READY_STORAGE |
  OTA_BOOT_READY_BUTTONS | OTA_BOOT_READY_BLE |
  OTA_BOOT_READY_EVENT_LOOP;

enum OtaBootHealthReason : uint8_t {
  OTA_BOOT_REASON_NONE = 0,
  OTA_BOOT_REASON_LAYOUT,
  OTA_BOOT_REASON_DISPLAY,
  OTA_BOOT_REASON_STORAGE,
  OTA_BOOT_REASON_BUTTONS,
  OTA_BOOT_REASON_BLE,
  OTA_BOOT_REASON_EVENT_LOOP,
  OTA_BOOT_REASON_TIMEOUT,
  OTA_BOOT_REASON_MARK_VALID,
  OTA_BOOT_REASON_SUPERVISOR,
};

enum OtaBootHealthPhase : uint8_t {
  OTA_BOOT_PHASE_INERT = 0,
  OTA_BOOT_PHASE_QUERY_UNKNOWN,
  OTA_BOOT_PHASE_MONITORING,
  OTA_BOOT_PHASE_MARK_VALID,
  OTA_BOOT_PHASE_VALID,
  OTA_BOOT_PHASE_ROLLBACK,
  OTA_BOOT_PHASE_HALTED,
};

enum OtaBootHealthAction : uint8_t {
  OTA_BOOT_ACTION_NONE = 0,
  OTA_BOOT_ACTION_MARK_VALID,
  OTA_BOOT_ACTION_ROLLBACK,
};

struct OtaBootHealthState {
  OtaBootHealthPhase phase;
  uint8_t readyBits;
  uint32_t deadlineMs;
  OtaBootHealthReason reason;
  bool actionIssued;
};

inline OtaBootHealthState otaBootHealthInitial() {
  OtaBootHealthState state = {};
  state.phase = OTA_BOOT_PHASE_INERT;
  return state;
}

inline bool otaBootHealthDeadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

inline const char* otaBootHealthReasonLabel(OtaBootHealthReason reason) {
  switch (reason) {
    case OTA_BOOT_REASON_LAYOUT: return "layout";
    case OTA_BOOT_REASON_DISPLAY: return "display";
    case OTA_BOOT_REASON_STORAGE: return "storage";
    case OTA_BOOT_REASON_BUTTONS: return "buttons";
    case OTA_BOOT_REASON_BLE: return "ble";
    case OTA_BOOT_REASON_EVENT_LOOP: return "event-loop";
    case OTA_BOOT_REASON_TIMEOUT: return "timeout";
    case OTA_BOOT_REASON_MARK_VALID: return "mark-valid";
    case OTA_BOOT_REASON_SUPERVISOR: return "supervisor";
    case OTA_BOOT_REASON_NONE: return "none";
  }
  return "unknown";
}

inline bool otaBootHealthReasonIsPersistable(uint8_t reason) {
  return reason >= OTA_BOOT_REASON_LAYOUT &&
    reason <= OTA_BOOT_REASON_SUPERVISOR;
}

inline uint32_t otaBootRollbackRecordChecksum(
  uint8_t version,
  OtaBootHealthReason reason,
  int32_t errorCode
) {
  // CRC-32/ISO-HDLC over an explicit, endian-stable version/reason/error
  // encoding. This is integrity for retained diagnostics, not authentication.
  const uint8_t bytes[6] = {
    version,
    static_cast<uint8_t>(reason),
    static_cast<uint8_t>(static_cast<uint32_t>(errorCode)),
    static_cast<uint8_t>(static_cast<uint32_t>(errorCode) >> 8),
    static_cast<uint8_t>(static_cast<uint32_t>(errorCode) >> 16),
    static_cast<uint8_t>(static_cast<uint32_t>(errorCode) >> 24),
  };
  uint32_t crc = 0xFFFFFFFFU;
  for (uint8_t byte : bytes) {
    crc ^= byte;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

inline bool otaBootRollbackRecordValid(
  uint32_t magic,
  uint8_t version,
  OtaBootHealthReason reason,
  int32_t errorCode,
  uint32_t checksum
) {
  return magic == OTA_BOOT_ROLLBACK_RECORD_MAGIC &&
    version == OTA_BOOT_ROLLBACK_RECORD_VERSION &&
    otaBootHealthReasonIsPersistable(static_cast<uint8_t>(reason)) &&
    checksum == otaBootRollbackRecordChecksum(version, reason, errorCode);
}

inline const char* otaBootHealthPhaseLabel(OtaBootHealthPhase phase) {
  switch (phase) {
    case OTA_BOOT_PHASE_INERT: return "ordinary";
    case OTA_BOOT_PHASE_QUERY_UNKNOWN: return "state-unknown";
    case OTA_BOOT_PHASE_MONITORING: return "pending";
    case OTA_BOOT_PHASE_MARK_VALID: return "validating";
    case OTA_BOOT_PHASE_VALID: return "valid";
    case OTA_BOOT_PHASE_ROLLBACK: return "rollback";
    case OTA_BOOT_PHASE_HALTED: return "halted";
  }
  return "unknown";
}

inline void otaBootHealthArm(
  OtaBootHealthState* state,
  OtaBootStateQuery query,
  uint32_t now,
  bool otaLayoutValid
) {
  if (!state) return;
  *state = otaBootHealthInitial();
  if (query == OTA_BOOT_QUERY_UNKNOWN) {
    state->phase = OTA_BOOT_PHASE_QUERY_UNKNOWN;
    return;
  }
  if (query != OTA_BOOT_QUERY_PENDING_VERIFY) return;
  state->deadlineMs = now + OTA_BOOT_HEALTH_TIMEOUT_MS;
  if (!otaLayoutValid) {
    state->phase = OTA_BOOT_PHASE_ROLLBACK;
    state->reason = OTA_BOOT_REASON_LAYOUT;
    return;
  }
  state->phase = OTA_BOOT_PHASE_MONITORING;
}

inline void otaBootHealthSignal(
  OtaBootHealthState* state,
  OtaBootReadyBit bit
) {
  if (!state || state->phase != OTA_BOOT_PHASE_MONITORING) return;
  state->readyBits |= static_cast<uint8_t>(bit) & OTA_BOOT_READY_ALL;
}

inline void otaBootHealthFail(
  OtaBootHealthState* state,
  OtaBootHealthReason reason
) {
  if (!state || state->phase != OTA_BOOT_PHASE_MONITORING) return;
  state->phase = OTA_BOOT_PHASE_ROLLBACK;
  state->reason = reason == OTA_BOOT_REASON_NONE
    ? OTA_BOOT_REASON_EVENT_LOOP : reason;
  state->actionIssued = false;
}

inline OtaBootHealthAction otaBootHealthNextAction(
  OtaBootHealthState* state,
  uint32_t now
) {
  if (!state) return OTA_BOOT_ACTION_NONE;
  if (state->phase == OTA_BOOT_PHASE_MONITORING) {
    // Expiry wins at the exact deadline. A healthy image must have been
    // observed and confirmed strictly before the 30-second boundary.
    if (otaBootHealthDeadlineReached(now, state->deadlineMs)) {
      state->phase = OTA_BOOT_PHASE_ROLLBACK;
      state->reason = OTA_BOOT_REASON_TIMEOUT;
      state->actionIssued = false;
    } else if ((state->readyBits & OTA_BOOT_READY_ALL) == OTA_BOOT_READY_ALL) {
      state->phase = OTA_BOOT_PHASE_MARK_VALID;
      state->actionIssued = true;
      return OTA_BOOT_ACTION_MARK_VALID;
    }
  }
  if (state->phase == OTA_BOOT_PHASE_ROLLBACK && !state->actionIssued) {
    state->actionIssued = true;
    return OTA_BOOT_ACTION_ROLLBACK;
  }
  return OTA_BOOT_ACTION_NONE;
}

// The caller must hold its supervisor lock while invoking this helper. A
// rollback action and the irreversible HALTED latch are claimed together, so
// a second loop/watchdog caller can never observe "action issued" and resume.
inline OtaBootHealthAction otaBootHealthClaimAction(
  OtaBootHealthState* state,
  uint32_t now
) {
  OtaBootHealthAction action = otaBootHealthNextAction(state, now);
  if (state && action == OTA_BOOT_ACTION_ROLLBACK) {
    state->phase = OTA_BOOT_PHASE_HALTED;
  }
  return action;
}

inline bool otaBootHealthNormalExecutionAllowed(
  const OtaBootHealthState* state
) {
  return state && state->phase != OTA_BOOT_PHASE_ROLLBACK &&
    state->phase != OTA_BOOT_PHASE_HALTED;
}

inline void otaBootHealthMarkValidResult(
  OtaBootHealthState* state,
  bool success
) {
  if (!state || state->phase != OTA_BOOT_PHASE_MARK_VALID) return;
  if (success) {
    state->phase = OTA_BOOT_PHASE_VALID;
    return;
  }
  state->phase = OTA_BOOT_PHASE_ROLLBACK;
  state->reason = OTA_BOOT_REASON_MARK_VALID;
  state->actionIssued = false;
}

// Adapter contract:
//   bool markValid();
//   void persistReason(OtaBootHealthReason, int32_t errorCode);
//   int markInvalidRollbackAndReboot();  // returns only on failure
//   void boundedDelay(uint32_t);
//   void restart();                      // expected not to return on device
//   void fatalStop();                    // expected not to return on device
template <typename Adapter>
inline void otaBootHealthRun(
  OtaBootHealthState* state,
  uint32_t now,
  Adapter* adapter
) {
  if (!state || !adapter) return;
  OtaBootHealthAction action = otaBootHealthClaimAction(state, now);
  if (action == OTA_BOOT_ACTION_MARK_VALID) {
    otaBootHealthMarkValidResult(state, adapter->markValid());
    action = otaBootHealthClaimAction(state, now);
  }
  if (action != OTA_BOOT_ACTION_ROLLBACK) return;

  // Record the reason before calling an API which normally never returns. If
  // it does return, replace the provisional zero with its exact numeric error.
  adapter->persistReason(state->reason, 0);
  int32_t error = adapter->markInvalidRollbackAndReboot();
  adapter->persistReason(state->reason, error);
  adapter->boundedDelay(OTA_BOOT_ROLLBACK_RESTART_DELAY_MS);
  adapter->restart();
  adapter->fatalStop();
}
