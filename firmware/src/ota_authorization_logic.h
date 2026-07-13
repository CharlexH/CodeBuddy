#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ota_manifest_logic.h"

constexpr char OTA_AUTHORIZATION_ACTION[] = "code-buddy-firmware-install-v1";
constexpr size_t OTA_AUTHORIZATION_MAX_BYTES = 1024;
constexpr uint32_t OTA_AUTHORIZATION_MAX_LIFETIME_SECONDS = 300;
constexpr size_t OTA_AUTHORIZATION_REPLAY_SLOTS = 4;

enum OtaAuthorizationResult : uint8_t {
  OTA_AUTHORIZATION_OK = 0,
  OTA_AUTHORIZATION_ARGUMENT_INVALID,
  OTA_AUTHORIZATION_FIELD_INVALID,
  OTA_AUTHORIZATION_DEVICE_MISMATCH,
  OTA_AUTHORIZATION_TIME_UNTRUSTED,
  OTA_AUTHORIZATION_NOT_YET_VALID,
  OTA_AUTHORIZATION_EXPIRED,
  OTA_AUTHORIZATION_REPLAY,
  OTA_AUTHORIZATION_SIGNATURE_INVALID,
};

struct OtaAuthorizationInput {
  const char* action;
  const char* device;
  uint32_t expiresAt;
  uint32_t generation;
  uint32_t issuedAt;
  const char* manifestUrl;
  const char* nonce;
  const char* signatureUrl;
  uint32_t sizeBytes;
  const char* version;
  const char* authorizationHex;
};

struct OtaAuthorizationReplayEntry {
  bool valid;
  uint32_t generation;
  uint32_t expiresAt;
  char nonce[49];
};

struct OtaAuthorizationReplayState {
  OtaAuthorizationReplayEntry entries[OTA_AUTHORIZATION_REPLAY_SLOTS];
  uint8_t nextSlot;
};

using OtaAuthorizationSignatureVerifier = bool (*)(
  const uint8_t*, size_t, const uint8_t*, size_t, void*
);

inline OtaAuthorizationReplayState otaAuthorizationReplayInitial() {
  OtaAuthorizationReplayState state = {};
  return state;
}

inline bool otaAuthorizationSignedOfferShapeValid(size_t fieldCount) {
  return fieldCount == 10;
}

inline bool otaAuthorizationMayReportRejection(OtaAuthorizationResult result) {
  return result == OTA_AUTHORIZATION_OK;
}

inline bool otaAuthorizationDeviceNameValid(const char* value) {
  if (!value || strlen(value) != 10 || memcmp(value, "Codex-", 6) != 0) return false;
  for (size_t i = 6; i < 10; ++i) {
    if (!otaAsciiDigit(value[i]) && !(value[i] >= 'A' && value[i] <= 'F')) return false;
  }
  return true;
}

inline bool otaAuthorizationNonceValid(const char* value) {
  if (!value) return false;
  size_t length = strnlen(value, 49);
  if (length < 24 || length > 48) return false;
  for (size_t i = 0; i < length; ++i) {
    if (!otaTokenChar(value[i])) return false;
  }
  return true;
}

inline bool otaAuthorizationFieldsValid(const OtaAuthorizationInput& input) {
  if (!input.action || strcmp(input.action, OTA_AUTHORIZATION_ACTION) != 0 ||
      !otaAuthorizationDeviceNameValid(input.device) ||
      !otaAuthorizationNonceValid(input.nonce) || !input.generation ||
      !input.version || !input.manifestUrl || !input.signatureUrl ||
      !input.authorizationHex || !input.sizeBytes ||
      input.sizeBytes > OTA_SLOT_CAPACITY_BYTES ||
      input.issuedAt >= input.expiresAt ||
      input.expiresAt - input.issuedAt > OTA_AUTHORIZATION_MAX_LIFETIME_SECONDS)
    return false;
  OtaSemanticVersion ignored = {};
  return otaParseSemanticVersion(input.version, &ignored) &&
    otaLocalHttpsUrlsShareEndpoint(
      input.manifestUrl, OTA_URL_MANIFEST,
      input.signatureUrl, OTA_URL_SIGNATURE
    );
}

inline size_t otaAuthorizationCanonicalBytes(
  const OtaAuthorizationInput& input,
  char* output,
  size_t outputCapacity
) {
  if (output && outputCapacity) output[0] = 0;
  if (!output || !outputCapacity || !otaAuthorizationFieldsValid(input)) return 0;
  int written = snprintf(
    output,
    outputCapacity,
    "{\"action\":\"%s\",\"device\":\"%s\",\"expiresAt\":%u,"
    "\"generation\":%u,\"issuedAt\":%u,\"manifestUrl\":\"%s\","
    "\"nonce\":\"%s\",\"signatureUrl\":\"%s\",\"sizeBytes\":%u,"
    "\"version\":\"%s\"}",
    input.action,
    input.device,
    static_cast<unsigned>(input.expiresAt),
    static_cast<unsigned>(input.generation),
    static_cast<unsigned>(input.issuedAt),
    input.manifestUrl,
    input.nonce,
    input.signatureUrl,
    static_cast<unsigned>(input.sizeBytes),
    input.version
  );
  if (written <= 0 || static_cast<size_t>(written) >= outputCapacity) {
    output[0] = 0;
    return 0;
  }
  return static_cast<size_t>(written);
}

inline uint8_t otaAuthorizationHexNibble(char value) {
  if (otaAsciiDigit(value)) return static_cast<uint8_t>(value - '0');
  return static_cast<uint8_t>(value - 'a' + 10);
}

inline bool otaAuthorizationDerIntegerValid(
  const uint8_t* der,
  size_t derLength,
  size_t* position
) {
  if (!der || !position || *position + 2 > derLength || der[(*position)++] != 0x02)
    return false;
  size_t length = der[(*position)++];
  if (length == 0 || length > 33 || *position + length > derLength) return false;
  const uint8_t* value = der + *position;
  if ((value[0] & 0x80U) != 0 ||
      (length > 1 && value[0] == 0 && (value[1] & 0x80U) == 0)) return false;
  bool nonZero = false;
  for (size_t i = 0; i < length; ++i) nonZero = nonZero || value[i] != 0;
  *position += length;
  return nonZero;
}

inline bool otaAuthorizationDecodeDerHex(
  const char* hex,
  uint8_t* output,
  size_t outputCapacity,
  size_t* outputLength
) {
  if (outputLength) *outputLength = 0;
  if (!hex || !output || !outputLength) return false;
  size_t hexLength = strnlen(hex, OTA_SIGNATURE_MAX_BYTES * 2 + 1);
  if (hexLength < 16 || hexLength > OTA_SIGNATURE_MAX_BYTES * 2 ||
      (hexLength & 1U) != 0 || hexLength / 2 > outputCapacity) return false;
  for (size_t i = 0; i < hexLength; ++i) {
    if (!otaLowerHex(hex[i])) return false;
  }
  size_t decodedLength = hexLength / 2;
  for (size_t i = 0; i < decodedLength; ++i) {
    output[i] = static_cast<uint8_t>(
      (otaAuthorizationHexNibble(hex[i * 2]) << 4) |
      otaAuthorizationHexNibble(hex[i * 2 + 1])
    );
  }
  if (output[0] != 0x30 || (output[1] & 0x80U) != 0 ||
      static_cast<size_t>(output[1]) + 2 != decodedLength) return false;
  size_t position = 2;
  if (!otaAuthorizationDerIntegerValid(output, decodedLength, &position) ||
      !otaAuthorizationDerIntegerValid(output, decodedLength, &position) ||
      position != decodedLength) return false;
  *outputLength = decodedLength;
  return true;
}

inline bool otaAuthorizationReplaySeen(
  OtaAuthorizationReplayState* state,
  const char* nonce,
  uint32_t generation,
  uint32_t nowEpoch
) {
  if (!state || !nonce || !generation) return false;
  for (size_t i = 0; i < OTA_AUTHORIZATION_REPLAY_SLOTS; ++i) {
    OtaAuthorizationReplayEntry& entry = state->entries[i];
    if (entry.valid && nowEpoch >= entry.expiresAt) entry = {};
    if (entry.valid && entry.generation == generation && strcmp(entry.nonce, nonce) == 0)
      return true;
  }
  return false;
}

inline void otaAuthorizationRemember(
  OtaAuthorizationReplayState* state,
  const OtaAuthorizationInput& input
) {
  size_t slot = state->nextSlot % OTA_AUTHORIZATION_REPLAY_SLOTS;
  OtaAuthorizationReplayEntry& entry = state->entries[slot];
  entry = {};
  entry.valid = true;
  entry.generation = input.generation;
  entry.expiresAt = input.expiresAt;
  memcpy(entry.nonce, input.nonce, strlen(input.nonce) + 1);
  state->nextSlot = static_cast<uint8_t>((slot + 1) % OTA_AUTHORIZATION_REPLAY_SLOTS);
}

inline OtaAuthorizationResult otaAuthorizationVerifyAndRemember(
  const OtaAuthorizationInput& input,
  const char* expectedDevice,
  bool timeTrusted,
  uint32_t nowEpoch,
  OtaAuthorizationSignatureVerifier verifier,
  void* verifierContext,
  OtaAuthorizationReplayState* replay
) {
  if (!expectedDevice || !verifier || !replay) return OTA_AUTHORIZATION_ARGUMENT_INVALID;
  if (!otaAuthorizationFieldsValid(input)) return OTA_AUTHORIZATION_FIELD_INVALID;
  if (!otaAuthorizationDeviceNameValid(expectedDevice) ||
      strcmp(input.device, expectedDevice) != 0) return OTA_AUTHORIZATION_DEVICE_MISMATCH;
  if (!timeTrusted) return OTA_AUTHORIZATION_TIME_UNTRUSTED;
  if (nowEpoch < input.issuedAt) return OTA_AUTHORIZATION_NOT_YET_VALID;
  if (nowEpoch >= input.expiresAt) return OTA_AUTHORIZATION_EXPIRED;
  if (otaAuthorizationReplaySeen(replay, input.nonce, input.generation, nowEpoch))
    return OTA_AUTHORIZATION_REPLAY;

  char canonical[OTA_AUTHORIZATION_MAX_BYTES] = {};
  size_t canonicalLength = otaAuthorizationCanonicalBytes(
    input, canonical, sizeof(canonical)
  );
  uint8_t signature[OTA_SIGNATURE_MAX_BYTES] = {};
  size_t signatureLength = 0;
  if (!canonicalLength || !otaAuthorizationDecodeDerHex(
        input.authorizationHex, signature, sizeof(signature), &signatureLength
      ) || !verifier(
        reinterpret_cast<const uint8_t*>(canonical), canonicalLength,
        signature, signatureLength, verifierContext
      )) return OTA_AUTHORIZATION_SIGNATURE_INVALID;
  otaAuthorizationRemember(replay, input);
  return OTA_AUTHORIZATION_OK;
}

inline bool otaAuthorizationVerifyThenAccept(
  const OtaAuthorizationInput& input,
  const char* expectedDevice,
  bool timeTrusted,
  uint32_t nowEpoch,
  OtaAuthorizationSignatureVerifier verifier,
  void* verifierContext,
  OtaAuthorizationReplayState* replay,
  const char* currentVersion,
  uint32_t nowMs,
  bool connected,
  bool approvalConflict,
  bool transferConflict,
  bool otherConflict,
  bool externalPower,
  uint8_t batteryPercent,
  OtaOfferState* output,
  OtaAuthorizationResult* authorizationResult
) {
  if (!authorizationResult || !output) {
    if (authorizationResult) *authorizationResult = OTA_AUTHORIZATION_ARGUMENT_INVALID;
    otaOfferReset(output);
    return false;
  }
  otaOfferReset(output);
  *authorizationResult = otaAuthorizationVerifyAndRemember(
    input, expectedDevice, timeTrusted, nowEpoch, verifier, verifierContext, replay
  );
  if (*authorizationResult != OTA_AUTHORIZATION_OK) return false;
  return otaOfferAcceptVerifiedBoundHint(
    input.nonce, input.generation, input.version, input.sizeBytes,
    input.manifestUrl, input.signatureUrl, currentVersion, nowMs, connected,
    approvalConflict, transferConflict, otherConflict, externalPower,
    batteryPercent, output
  );
}
