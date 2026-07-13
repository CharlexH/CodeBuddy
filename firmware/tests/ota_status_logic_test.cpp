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

  expect(otaStatusConfirmationPhase(false) == OTA_STATUS_AWAIT_CONFIRM,
         "Ask policy reports an explicit confirmation wait");
  expect(otaStatusConfirmationPhase(true) == OTA_STATUS_ACCEPTED,
         "Direct policy must never project await-confirm");

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

  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_WIFI), "wifi") == 0,
         "Wi-Fi readiness failures are actionable");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_TRUST), "trust") == 0,
         "TLS and signature failures are trust failures");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_MANIFEST), "manifest") == 0,
         "manifest transport and schema failures are explicit");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_VERSION), "version") == 0,
         "version failures are explicit");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_DOWNLOAD), "download") == 0,
         "firmware transport and flash failures are download failures");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_HASH), "hash") == 0,
         "digest mismatch is explicit");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_POWER), "power") == 0,
         "power gate failures are explicit");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_TIMEOUT), "timeout") == 0,
         "deadlines are explicit");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_ROLLBACK), "rollback") == 0,
         "slot and boot-selection failures are rollback failures");
  expect(std::strcmp(otaStatusFailureLabel(OTA_UPDATE_FAILURE_NONE), "download") == 0,
         "unknown internal failures fail closed to a whitelisted code");

  OtaStatusProjection projection = otaStatusProjectUpdate(
    OTA_PHASE_SET_BOOT, false, OTA_UPDATE_FAILURE_NONE
  );
  expect(projection.phase == OTA_STATUS_READBACK && projection.error[0] == 0,
         "pre-commit set-boot reports the last truthful reversible phase");
  projection = otaStatusProjectUpdate(
    OTA_PHASE_RESTART, true, OTA_UPDATE_FAILURE_NONE
  );
  expect(projection.phase == OTA_STATUS_BOOT_COMMITTED,
         "committed restart reports boot-committed");
  projection = otaStatusProjectUpdate(
    OTA_PHASE_RESTARTING, true, OTA_UPDATE_FAILURE_NONE
  );
  expect(projection.phase == OTA_STATUS_RESTARTING,
         "restarting remains restarting when cancel cannot apply");
  projection = otaStatusProjectUpdate(
    OTA_PHASE_ERROR, false, OTA_UPDATE_FAILURE_HASH
  );
  expect(projection.phase == OTA_STATUS_ERROR &&
         std::strcmp(projection.error, "hash") == 0,
         "terminal errors preserve their actionable code");
  projection = otaStatusProjectUpdate(
    OTA_PHASE_CANCELLED, false, OTA_UPDATE_FAILURE_NONE
  );
  expect(projection.phase == OTA_STATUS_CANCELLED &&
         std::strcmp(projection.error, "cancelled") == 0,
         "repeated cancel can report the existing cancelled terminal state");

  char json[256] = {};
  size_t length = otaStatusBuildJson(
    json, sizeof(json), nonce, 7, OTA_STATUS_READBACK, 90,
    "0.1.5", "monitoring", "", false
  );
  expect(length > 0 && length < sizeof(json), "bounded status JSON is built");
  expect(std::strstr(json, "\"cmd\":\"ota_status\"") != nullptr,
         "status command is explicit");
  expect(std::strstr(json, "\"generation\":7") != nullptr,
         "generation is bound");
  expect(std::strstr(json, "\"cancel_applied\":false") != nullptr,
         "ordinary status explicitly reports cancellation was not applied");
  expect(std::strstr(json, "https://") == nullptr &&
         std::strstr(json, "manifest") == nullptr &&
         std::strstr(json, "signature") == nullptr &&
         std::strstr(json, "password") == nullptr,
         "status never carries endpoint or credential material");
  expect(otaStatusBuildJson(
      json, sizeof(json), nonce, 7, OTA_STATUS_ERROR, 0,
      "0.1.5", "valid", "https://secret", false
    ) == 0, "free-form secret-bearing errors are rejected");

  length = otaStatusBuildJson(
    json, sizeof(json), nonce, 7, OTA_STATUS_CANCELLED, 0,
    "0.1.5", "ordinary", "cancelled", true
  );
  expect(length > 0 && std::strstr(json, "\"cancel_applied\":true") != nullptr,
         "accepted cancellation is explicitly acknowledged");

  char maxJson[384] = {};
  expect(otaStatusBuildJson(
      maxJson, sizeof(maxJson),
      "abcdefghijklmnopqrstuvwxABCDEFGHIJKLMNOPQRSTUVWX", UINT32_MAX,
      OTA_STATUS_BOOT_COMMITTED, 100,
      "123456789012345678901234567890123456789012345678901234567890123",
      "monitoring", "rollback", false
    ) > 0, "maximum accepted identity fields fit the device status buffer");

  expect(std::strcmp(otaStatusHealthLabel("pending"), "monitoring") == 0,
         "boot pending is normalized");
  expect(std::strcmp(otaStatusHealthLabel("state-unknown"), "error") == 0,
         "unknown boot state is normalized");

  if (failures) return 1;
  std::puts("ota status logic tests passed");
  return 0;
}
