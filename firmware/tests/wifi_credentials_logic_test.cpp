#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "wifi_credentials_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

class FakeStore : public WifiCredentialStore {
 public:
  bool beginOk = true;
  int failPutCall = -1;
  int failGetCall = -1;
  int putCalls = 0;
  int getCalls = 0;
  std::map<std::string, std::string> strings;
  std::map<std::string, uint32_t> numbers;

  bool begin(bool) override { return beginOk; }
  void end() override {}
  bool putString(const char* key, const char* value) override {
    if (++putCalls == failPutCall) return false;
    strings[key] = value;
    return true;
  }
  bool getString(const char* key, char* out, size_t size) override {
    if (++getCalls == failGetCall || !strings.count(key) || size == 0) return false;
    const std::string& value = strings[key];
    size_t n = value.size() < size - 1 ? value.size() : size - 1;
    memcpy(out, value.data(), n); out[n] = 0;
    return true;
  }
  bool putUInt(const char* key, uint32_t value) override {
    if (++putCalls == failPutCall) return false;
    numbers[key] = value; return true;
  }
  bool getUInt(const char* key, uint32_t* value) override {
    if (++getCalls == failGetCall || !numbers.count(key) || !value) return false;
    *value = numbers[key]; return true;
  }
  bool putUChar(const char* key, uint8_t value) override {
    if (++putCalls == failPutCall) return false;
    numbers[key] = value; return true;
  }
  bool getUChar(const char* key, uint8_t* value) override {
    if (++getCalls == failGetCall || !numbers.count(key) || !value) return false;
    *value = (uint8_t)numbers[key]; return true;
  }
  bool remove(const char* key) override {
    strings.erase(key); numbers.erase(key); return true;
  }
};

static WifiCredentials oldCredentials() {
  WifiCredentials old = {};
  strcpy(old.ssid, "OldNetwork");
  strcpy(old.password, "oldpassword");
  old.generation = 3;
  old.slot = 0;
  old.valid = true;
  return old;
}

static void seedOld(FakeStore& store) {
  store.strings["s0_ssid"] = "OldNetwork";
  store.strings["s0_pass"] = "v1:oldpassword";
  store.numbers["s0_gen"] = 3;
  store.numbers["s0_valid"] = 1;
  store.numbers["active"] = 0;
  store.numbers["migrated"] = 1;
}

int main() {
  WifiCredentials next = {};
  strcpy(next.ssid, "NewNetwork");
  strcpy(next.password, "newpassword");
  next.valid = true;

  FakeStore success;
  seedOld(success);
  WifiCredentials committed = {};
  expect_true(wifiCredentialCommit(success, oldCredentials(), next, &committed),
              "complete staged write and read-back should commit");
  expect_true(committed.slot == 1 && committed.generation == 4,
              "transaction should activate the other slot with a new generation");
  WifiCredentials loaded = {};
  expect_true(wifiCredentialLoad(success, &loaded), "committed slot should load");
  expect_true(strcmp(loaded.ssid, "NewNetwork") == 0,
              "committed credentials should become active");

  FakeStore beginFailure;
  beginFailure.beginOk = false;
  expect_true(!wifiCredentialCommit(beginFailure, oldCredentials(), next, &committed),
              "namespace begin failure must abort the transaction");

  for (int failPut = 1; failPut <= 7; ++failPut) {
    FakeStore failure;
    seedOld(failure);
    failure.failPutCall = failPut;
    expect_true(!wifiCredentialCommit(failure, oldCredentials(), next, &committed),
                "every failed staged/marker write must fail the transaction");
    WifiCredentials stillOld = {};
    failure.failPutCall = -1;
    failure.getCalls = 0;
    expect_true(wifiCredentialLoad(failure, &stillOld),
                "previous active configuration must remain readable");
    expect_true(strcmp(stillOld.ssid, "OldNetwork") == 0,
                "partial writes must not replace the previous configuration");
    expect_true(strcmp(stillOld.password, "oldpassword") == 0,
                "partial writes must preserve the previous password");
  }

  for (int failGet = 1; failGet <= 7; ++failGet) {
    FakeStore readFailure;
    seedOld(readFailure);
    readFailure.failGetCall = failGet;
    expect_true(!wifiCredentialCommit(readFailure, oldCredentials(), next, &committed),
                "every read-back failure must abort activation");
    readFailure.failGetCall = -1;
    readFailure.getCalls = 0;
    expect_true(wifiCredentialLoad(readFailure, &loaded) &&
                strcmp(loaded.ssid, "OldNetwork") == 0 &&
                strcmp(loaded.password, "oldpassword") == 0,
                "read-back failure must preserve the prior active slot");
  }

  FakeStore migration;
  migration.strings["ssid"] = "Legacy";
  migration.strings["password"] = "legacypass";
  WifiCredentials legacy = oldCredentials(); legacy.slot = 0xff; legacy.generation = 0;
  expect_true(wifiCredentialCommit(migration, legacy, next, &committed),
              "legacy credentials should migrate transactionally");
  expect_true(wifiCredentialMigrationComplete(migration),
              "successful commit should set migration-complete marker");
  expect_true(!migration.strings.count("ssid") && !migration.strings.count("password"),
              "successful migration should delete legacy credential keys");

  FakeStore corruptAfterMigration;
  corruptAfterMigration.numbers["migrated"] = 1;
  corruptAfterMigration.numbers["active"] = 1;
  corruptAfterMigration.strings["ssid"] = "StaleLegacy";
  corruptAfterMigration.strings["password"] = "stalepassword";
  expect_true(!wifiCredentialLoad(corruptAfterMigration, &loaded),
              "corrupt transactional slot should fail loading");
  expect_true(!wifiCredentialMayUseLegacy(corruptAfterMigration),
              "migration marker must forbid fallback to stale legacy keys");

  uint8_t secret[64]; memset(secret, 0xa5, sizeof(secret));
  wifiSecureZero(secret, sizeof(secret));
  for (size_t i = 0; i < sizeof(secret); ++i) {
    expect_true(secret[i] == 0, "full password buffer must be explicitly zeroized");
  }
  return 0;
}
