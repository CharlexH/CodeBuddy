#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static constexpr uint8_t TOKEN_HEARTBEAT_SAMPLE_COUNT = 64;
static constexpr size_t TOKEN_HEARTBEAT_ENCODED_LENGTH = 86;
static constexpr uint32_t TOKEN_HEARTBEAT_WINDOW_MS = 20000;
static constexpr uint8_t TOKEN_HEARTBEAT_MAX_HEIGHT = 7;

struct TokenHeartbeatState {
  uint8_t intensities[TOKEN_HEARTBEAT_SAMPLE_COUNT];
  uint32_t receivedAtMs;
  bool valid;
};

inline int8_t tokenHeartbeatBase64UrlValue(char character) {
  if (character >= 'A' && character <= 'Z') return character - 'A';
  if (character >= 'a' && character <= 'z') return character - 'a' + 26;
  if (character >= '0' && character <= '9') return character - '0' + 52;
  if (character == '-') return 62;
  if (character == '_') return 63;
  return -1;
}

inline bool tokenHeartbeatDecode(
  const char* encoded,
  uint8_t output[TOKEN_HEARTBEAT_SAMPLE_COUNT]
) {
  if (!encoded || !output || strlen(encoded) != TOKEN_HEARTBEAT_ENCODED_LENGTH) {
    return false;
  }

  uint8_t decoded[TOKEN_HEARTBEAT_SAMPLE_COUNT] = {};
  for (size_t encodedIndex = 0, outputIndex = 0;
       encodedIndex < TOKEN_HEARTBEAT_ENCODED_LENGTH - 2;
       encodedIndex += 4, outputIndex += 3) {
    const int8_t a = tokenHeartbeatBase64UrlValue(encoded[encodedIndex]);
    const int8_t b = tokenHeartbeatBase64UrlValue(encoded[encodedIndex + 1]);
    const int8_t c = tokenHeartbeatBase64UrlValue(encoded[encodedIndex + 2]);
    const int8_t d = tokenHeartbeatBase64UrlValue(encoded[encodedIndex + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0) return false;
    decoded[outputIndex] = static_cast<uint8_t>((a << 2) | (b >> 4));
    decoded[outputIndex + 1] = static_cast<uint8_t>((b << 4) | (c >> 2));
    decoded[outputIndex + 2] = static_cast<uint8_t>((c << 6) | d);
  }

  const int8_t a = tokenHeartbeatBase64UrlValue(
    encoded[TOKEN_HEARTBEAT_ENCODED_LENGTH - 2]
  );
  const int8_t b = tokenHeartbeatBase64UrlValue(
    encoded[TOKEN_HEARTBEAT_ENCODED_LENGTH - 1]
  );
  if (a < 0 || b < 0 || (b & 0x0f) != 0) return false;
  decoded[TOKEN_HEARTBEAT_SAMPLE_COUNT - 1] = static_cast<uint8_t>(
    (a << 2) | (b >> 4)
  );
  memcpy(output, decoded, sizeof(decoded));
  return true;
}

inline bool tokenHeartbeatApplyEncoded(
  TokenHeartbeatState* state,
  const char* encoded,
  uint32_t receivedAtMs
) {
  if (!state) return false;
  TokenHeartbeatState next = {};
  if (!tokenHeartbeatDecode(encoded, next.intensities)) return false;
  next.receivedAtMs = receivedAtMs;
  next.valid = true;
  *state = next;
  return true;
}

inline void tokenHeartbeatAgedIntensities(
  const TokenHeartbeatState& state,
  uint32_t nowMs,
  uint8_t output[TOKEN_HEARTBEAT_SAMPLE_COUNT]
) {
  if (!output) return;
  memset(output, 0, TOKEN_HEARTBEAT_SAMPLE_COUNT);
  if (!state.valid) return;

  const uint32_t elapsedMs = nowMs - state.receivedAtMs;
  if (elapsedMs >= TOKEN_HEARTBEAT_WINDOW_MS) return;
  const uint8_t elapsedBins = static_cast<uint8_t>(
    (static_cast<uint64_t>(elapsedMs) * TOKEN_HEARTBEAT_SAMPLE_COUNT) /
    TOKEN_HEARTBEAT_WINDOW_MS
  );
  const uint8_t remaining = TOKEN_HEARTBEAT_SAMPLE_COUNT - elapsedBins;
  memcpy(output, state.intensities + elapsedBins, remaining);
}

inline constexpr uint8_t tokenHeartbeatIntensityToHeight(uint8_t intensity) {
  return intensity == 0
    ? 0
    : static_cast<uint8_t>(
      (static_cast<uint16_t>(intensity) * TOKEN_HEARTBEAT_MAX_HEIGHT + 254) /
      255
    );
}
