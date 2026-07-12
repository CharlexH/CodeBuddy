#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct WifiCredentials {
  char ssid[33];
  char password[64];
  uint32_t generation;
  uint8_t slot;
  bool valid;
};

class WifiCredentialStore {
 public:
  virtual ~WifiCredentialStore() {}
  virtual bool begin(bool readOnly) = 0;
  virtual void end() = 0;
  virtual bool putString(const char* key, const char* value) = 0;
  virtual bool getString(const char* key, char* out, size_t size) = 0;
  virtual bool putUInt(const char* key, uint32_t value) = 0;
  virtual bool getUInt(const char* key, uint32_t* value) = 0;
  virtual bool putUChar(const char* key, uint8_t value) = 0;
  virtual bool getUChar(const char* key, uint8_t* value) = 0;
};

inline void wifiCredentialKey(char* out, size_t outSize, uint8_t slot, const char* field) {
  snprintf(out, outSize, "s%u_%s", slot, field);
}

inline void wifiCredentialRollback(
  WifiCredentialStore& store,
  uint8_t targetSlot,
  const WifiCredentials& current
) {
  char key[16];
  wifiCredentialKey(key, sizeof(key), targetSlot, "valid");
  store.putUChar(key, 0);
  store.putUChar("active", current.valid && current.slot < 2 ? current.slot : 0xff);
}

inline bool wifiCredentialLoad(WifiCredentialStore& store, WifiCredentials* out) {
  if (!out || !store.begin(true)) return false;
  WifiCredentials loaded = {};
  uint8_t active = 0xff;
  uint8_t valid = 0;
  char ssidKey[16], passKey[16], genKey[16], validKey[16];
  bool ok = store.getUChar("active", &active) && active < 2;
  if (ok) {
    wifiCredentialKey(ssidKey, sizeof(ssidKey), active, "ssid");
    wifiCredentialKey(passKey, sizeof(passKey), active, "pass");
    wifiCredentialKey(genKey, sizeof(genKey), active, "gen");
    wifiCredentialKey(validKey, sizeof(validKey), active, "valid");
    char encodedPassword[67] = {};
    ok = store.getUChar(validKey, &valid) && valid == 1 &&
         store.getString(ssidKey, loaded.ssid, sizeof(loaded.ssid)) &&
         store.getString(passKey, encodedPassword, sizeof(encodedPassword)) &&
         store.getUInt(genKey, &loaded.generation) &&
         strncmp(encodedPassword, "v1:", 3) == 0;
    if (ok) {
      size_t n = strnlen(encodedPassword + 3, sizeof(loaded.password) - 1);
      memcpy(loaded.password, encodedPassword + 3, n);
      loaded.password[n] = 0;
      loaded.slot = active;
      loaded.valid = true;
    }
  }
  store.end();
  if (!ok) return false;
  *out = loaded;
  return true;
}

inline bool wifiCredentialCommit(
  WifiCredentialStore& store,
  const WifiCredentials& current,
  const WifiCredentials& candidate,
  WifiCredentials* committed
) {
  if (!committed || !candidate.valid || !store.begin(false)) return false;
  uint8_t target = current.valid && current.slot < 2 ? (uint8_t)(1 - current.slot) : 0;
  char ssidKey[16], passKey[16], genKey[16], validKey[16];
  wifiCredentialKey(ssidKey, sizeof(ssidKey), target, "ssid");
  wifiCredentialKey(passKey, sizeof(passKey), target, "pass");
  wifiCredentialKey(genKey, sizeof(genKey), target, "gen");
  wifiCredentialKey(validKey, sizeof(validKey), target, "valid");
  char encodedPassword[67];
  snprintf(encodedPassword, sizeof(encodedPassword), "v1:%s", candidate.password);
  uint32_t generation = current.valid ? current.generation + 1 : 1;
  uint8_t marker = 0xff;
  char verifySsid[33] = {};
  char verifyPassword[67] = {};
  uint32_t verifyGeneration = 0;

  bool ok = store.putUChar(validKey, 0) &&
            store.getUChar(validKey, &marker) && marker == 0 &&
            store.putString(ssidKey, candidate.ssid) &&
            store.putString(passKey, encodedPassword) &&
            store.putUInt(genKey, generation) &&
            store.getString(ssidKey, verifySsid, sizeof(verifySsid)) &&
            store.getString(passKey, verifyPassword, sizeof(verifyPassword)) &&
            store.getUInt(genKey, &verifyGeneration) &&
            strcmp(verifySsid, candidate.ssid) == 0 &&
            strcmp(verifyPassword, encodedPassword) == 0 &&
            verifyGeneration == generation &&
            store.putUChar(validKey, 1) &&
            store.getUChar(validKey, &marker) && marker == 1 &&
            store.putUChar("active", target) &&
            store.getUChar("active", &marker) && marker == target;
  if (!ok) {
    wifiCredentialRollback(store, target, current);
    store.end();
    return false;
  }

  *committed = candidate;
  committed->slot = target;
  committed->generation = generation;
  committed->valid = true;
  store.end();
  return true;
}
