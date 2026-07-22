#include <assert.h>
#include <string.h>

#include "token_heartbeat_logic.h"

int main() {
  static constexpr char kEncodedRange[] =
    "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v"
    "MDEyMzQ1Njc4OTo7PD0-Pw";
  static_assert(sizeof(kEncodedRange) - 1 == TOKEN_HEARTBEAT_ENCODED_LENGTH,
                "token20v1 must be exactly 86 ASCII characters");

  TokenHeartbeatState state = {};
  assert(tokenHeartbeatApplyEncoded(&state, kEncodedRange, 1000));
  assert(state.valid);
  assert(state.receivedAtMs == 1000);
  for (uint8_t index = 0; index < TOKEN_HEARTBEAT_SAMPLE_COUNT; ++index) {
    assert(state.intensities[index] == index);
  }

  TokenHeartbeatState beforeInvalid = state;
  char invalidAlphabet[TOKEN_HEARTBEAT_ENCODED_LENGTH + 1] = {};
  memcpy(invalidAlphabet, kEncodedRange, TOKEN_HEARTBEAT_ENCODED_LENGTH + 1);
  invalidAlphabet[17] = '+';
  assert(!tokenHeartbeatApplyEncoded(&state, invalidAlphabet, 2000));
  assert(memcmp(&state, &beforeInvalid, sizeof(state)) == 0);

  assert(!tokenHeartbeatApplyEncoded(&state, "short", 2000));
  assert(memcmp(&state, &beforeInvalid, sizeof(state)) == 0);

  uint8_t aged[TOKEN_HEARTBEAT_SAMPLE_COUNT] = {};
  tokenHeartbeatAgedIntensities(state, 1313, aged);
  assert(aged[0] == 1);
  assert(aged[62] == 63);
  assert(aged[63] == 0);

  tokenHeartbeatAgedIntensities(state, 21000, aged);
  for (uint8_t intensity : aged) assert(intensity == 0);

  TokenHeartbeatState disconnected = state;
  tokenHeartbeatAgedIntensities(disconnected, 21000, aged);
  for (uint8_t intensity : aged) assert(intensity == 0);
  assert(disconnected.valid && disconnected.receivedAtMs == 1000);

  assert(tokenHeartbeatIntensityToHeight(0) == 0);
  assert(tokenHeartbeatIntensityToHeight(1) <= TOKEN_HEARTBEAT_MAX_HEIGHT);
  assert(tokenHeartbeatIntensityToHeight(255) == TOKEN_HEARTBEAT_MAX_HEIGHT);

  TokenHeartbeatState oldHost = {};
  assert(!oldHost.valid);
  tokenHeartbeatAgedIntensities(oldHost, 999999, aged);
  for (uint8_t intensity : aged) assert(intensity == 0);
  return 0;
}
