#include <cstdio>
#include <cstring>

#include "ota_status_logic.h"

static int failures = 0;
static void expect(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

int main() {
  const char* nonce = "abcdefghijklmnopqrstuvwx";
  expect(otaStatusNonceValid(nonce), "24-byte URL-safe nonce is valid");
  expect(!otaStatusNonceValid("short"), "short nonce is rejected");
  expect(!otaStatusNonceValid("abcdefghijklmnopqrstuvw!"), "punctuation is rejected");

  OtaStatusCadence cadence = otaStatusCadenceInitial();
  expect(otaStatusShouldEmit(&cadence, OTA_STATUS_AWAIT_CONFIRM, 0, ""),
         "first phase emits");
  expect(!otaStatusShouldEmit(&cadence, OTA_STATUS_AWAIT_CONFIRM, 9, ""),
         "sub-ten-percent progress is suppressed");
  expect(otaStatusShouldEmit(&cadence, OTA_STATUS_DOWNLOAD, 10, ""),
         "phase transition emits");
  expect(!otaStatusShouldEmit(&cadence, OTA_STATUS_DOWNLOAD, 19, ""),
         "same ten-percent bucket is suppressed");
  expect(otaStatusShouldEmit(&cadence, OTA_STATUS_DOWNLOAD, 20, ""),
         "next ten-percent bucket emits");
  expect(otaStatusShouldEmit(&cadence, OTA_STATUS_ERROR, 20, "hash"),
         "error transition emits");
  expect(!otaStatusShouldEmit(&cadence, OTA_STATUS_ERROR, 20, "hash"),
         "identical error is suppressed");

  char json[256] = {};
  size_t length = otaStatusBuildJson(
    json, sizeof(json), nonce, 7, OTA_STATUS_READBACK, 90,
    "0.1.5", "monitoring", ""
  );
  expect(length > 0 && length < sizeof(json), "bounded status JSON is built");
  expect(std::strstr(json, "\"cmd\":\"ota_status\"") != nullptr,
         "status command is explicit");
  expect(std::strstr(json, "\"generation\":7") != nullptr,
         "generation is bound");
  expect(std::strstr(json, "https://") == nullptr &&
         std::strstr(json, "manifest") == nullptr &&
         std::strstr(json, "signature") == nullptr &&
         std::strstr(json, "password") == nullptr,
         "status never carries endpoint or credential material");
  expect(otaStatusBuildJson(
      json, sizeof(json), nonce, 7, OTA_STATUS_ERROR, 0,
      "0.1.5", "valid", "https://secret"
    ) == 0, "free-form secret-bearing errors are rejected");

  expect(std::strcmp(otaStatusHealthLabel("pending"), "monitoring") == 0,
         "boot pending is normalized");
  expect(std::strcmp(otaStatusHealthLabel("state-unknown"), "error") == 0,
         "unknown boot state is normalized");

  if (failures) return 1;
  std::puts("ota status logic tests passed");
  return 0;
}
