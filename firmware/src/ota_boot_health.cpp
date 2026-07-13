#include "ota_boot_health.h"

#include <esp_attr.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) || \
    !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "Code Buddy OTA requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE"
#endif

namespace {

constexpr uint32_t ROLLBACK_RECORD_MAGIC = 0x43425242;  // "CBRB"
constexpr uint32_t EXPECTED_OTADATA_OFFSET = 0xE000;
constexpr uint32_t EXPECTED_OTADATA_SIZE = 0x2000;
constexpr uint32_t EXPECTED_OTA0_OFFSET = 0x10000;
constexpr uint32_t EXPECTED_OTA1_OFFSET = 0x340000;
constexpr uint32_t EXPECTED_OTA_SLOT_SIZE = 0x330000;

struct RollbackRecord {
  uint32_t magic;
  uint8_t reason;
  uint8_t reasonInverse;
  uint16_t reserved;
};

RTC_DATA_ATTR RollbackRecord retainedRollback = {};
OtaBootHealthState supervisor = otaBootHealthInitial();
OtaBootHealthReason observedRollbackReason = OTA_BOOT_REASON_NONE;
int lastStateQueryError = 0;
portMUX_TYPE supervisorMux = portMUX_INITIALIZER_UNLOCKED;

bool validReasonByte(uint8_t reason) {
  return reason >= OTA_BOOT_REASON_LAYOUT &&
    reason <= OTA_BOOT_REASON_SUPERVISOR;
}

void observeAndClearRetainedReason() {
  if (retainedRollback.magic == ROLLBACK_RECORD_MAGIC &&
      static_cast<uint8_t>(~retainedRollback.reason) ==
        retainedRollback.reasonInverse &&
      validReasonByte(retainedRollback.reason)) {
    observedRollbackReason =
      static_cast<OtaBootHealthReason>(retainedRollback.reason);
  }
  retainedRollback.magic = 0;
  retainedRollback.reason = 0;
  retainedRollback.reasonInverse = 0;
  retainedRollback.reserved = 0;
}

void retainRollbackReason(OtaBootHealthReason reason) {
  const uint8_t encoded = validReasonByte(static_cast<uint8_t>(reason))
    ? static_cast<uint8_t>(reason)
    : static_cast<uint8_t>(OTA_BOOT_REASON_EVENT_LOOP);
  retainedRollback.reason = encoded;
  retainedRollback.reasonInverse = static_cast<uint8_t>(~encoded);
  retainedRollback.reserved = 0;
  // Commit marker last so a reset during this tiny write is fail-closed for
  // diagnostics without ever blocking the actual rollback.
  __sync_synchronize();
  retainedRollback.magic = ROLLBACK_RECORD_MAGIC;
  __sync_synchronize();
}

bool exactOtaLayoutPresent() {
  const esp_partition_t* otadata = esp_partition_find_first(
    ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  const esp_partition_t* ota0 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* ota1 = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  return otadata && ota0 && ota1 && esp_ota_get_app_partition_count() == 2 &&
    otadata->address == EXPECTED_OTADATA_OFFSET &&
    otadata->size == EXPECTED_OTADATA_SIZE &&
    ota0->address == EXPECTED_OTA0_OFFSET &&
    ota0->size == EXPECTED_OTA_SLOT_SIZE &&
    ota1->address == EXPECTED_OTA1_OFFSET &&
    ota1->size == EXPECTED_OTA_SLOT_SIZE;
}

OtaBootStateQuery runningImageStateQuery() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) {
    lastStateQueryError = ESP_ERR_NOT_FOUND;
    return OTA_BOOT_QUERY_UNKNOWN;
  }
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t result = esp_ota_get_state_partition(running, &state);
  if (result == ESP_OK) {
    lastStateQueryError = ESP_OK;
    return state == ESP_OTA_IMG_PENDING_VERIFY
      ? OTA_BOOT_QUERY_PENDING_VERIFY
      : OTA_BOOT_QUERY_NON_PENDING;
  }
  // A factory/USB image may legitimately have no otadata state. It must stay
  // inert; later OTA staging independently refuses unexpected query errors.
  if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_NOT_SUPPORTED) {
    lastStateQueryError = result;
    return OTA_BOOT_QUERY_NON_PENDING;
  }
  lastStateQueryError = result;
  return OTA_BOOT_QUERY_UNKNOWN;
}

struct EspBootHealthAdapter {
  bool markValid() {
    esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result != ESP_OK) {
      lastStateQueryError = result;
      return false;
    }
    return true;
  }

  void persistReason(OtaBootHealthReason reason) {
    retainRollbackReason(reason);
  }

  int markInvalidRollbackAndReboot() {
    return esp_ota_mark_app_invalid_rollback_and_reboot();
  }

  void restart() { esp_restart(); }

  [[noreturn]] void fatalStop() {
    abort();
  }
};

[[noreturn]] void performRollback(OtaBootHealthReason reason) {
  EspBootHealthAdapter adapter;
  adapter.persistReason(reason);
  (void)adapter.markInvalidRollbackAndReboot();
  // Returning from the rollback API means it failed to reboot. The supervisor
  // has already latched HALTED, so no other task can resume or mark valid.
  for (uint8_t i = 0; i < OTA_BOOT_RESTART_FALLBACK_ATTEMPTS; ++i) {
    adapter.restart();
  }
  adapter.fatalStop();
}

void bootHealthWatchdogTask(void*) {
  // This task is independent of display, filesystem, BLE, Wi-Fi, and loop().
  // It keeps the active 30-second deadline enforceable even if setup blocks.
  for (;;) {
    uint32_t deadline = 0;
    OtaBootHealthPhase phase;
    portENTER_CRITICAL(&supervisorMux);
    deadline = supervisor.deadlineMs;
    phase = supervisor.phase;
    portEXIT_CRITICAL(&supervisorMux);
    if (phase != OTA_BOOT_PHASE_MONITORING) break;
    uint32_t now = millis();
    if (otaBootHealthDeadlineReached(now, deadline)) {
      otaBootHealthPoll(now);
      break;
    }
    uint32_t remaining = deadline - now;
    TickType_t wait = pdMS_TO_TICKS(remaining > 250 ? 250 : remaining);
    vTaskDelay(wait ? wait : 1);
  }
  vTaskDelete(nullptr);
}

}  // namespace

void otaBootHealthArmEarly(uint32_t nowMs) {
  observeAndClearRetainedReason();
  OtaBootStateQuery query = runningImageStateQuery();
  bool layoutPresent = exactOtaLayoutPresent();
  portENTER_CRITICAL(&supervisorMux);
  otaBootHealthArm(&supervisor, query, nowMs, layoutPresent);
  bool monitoring = supervisor.phase == OTA_BOOT_PHASE_MONITORING;
  portEXIT_CRITICAL(&supervisorMux);
  otaBootHealthPoll(nowMs);
  if (monitoring && xTaskCreatePinnedToCore(
      bootHealthWatchdogTask, "ota-boot-health", 3072, nullptr,
      configMAX_PRIORITIES - 2, nullptr, 0) != pdPASS) {
    otaBootHealthCriticalFailure(OTA_BOOT_REASON_SUPERVISOR);
  }
}

void otaBootHealthReady(OtaBootReadyBit bit) {
  portENTER_CRITICAL(&supervisorMux);
  otaBootHealthSignal(&supervisor, bit);
  portEXIT_CRITICAL(&supervisorMux);
}

void otaBootHealthCriticalFailure(OtaBootHealthReason reason) {
  portENTER_CRITICAL(&supervisorMux);
  otaBootHealthFail(&supervisor, reason);
  portEXIT_CRITICAL(&supervisorMux);
  otaBootHealthPoll(millis());
}

void otaBootHealthPoll(uint32_t nowMs) {
  EspBootHealthAdapter adapter;
  portENTER_CRITICAL(&supervisorMux);
  OtaBootHealthAction action = otaBootHealthNextAction(&supervisor, nowMs);
  OtaBootHealthReason rollbackReason = supervisor.reason;
  portEXIT_CRITICAL(&supervisorMux);

  if (action == OTA_BOOT_ACTION_MARK_VALID) {
    bool success = adapter.markValid();
    portENTER_CRITICAL(&supervisorMux);
    otaBootHealthMarkValidResult(&supervisor, success);
    action = otaBootHealthNextAction(&supervisor, nowMs);
    rollbackReason = supervisor.reason;
    portEXIT_CRITICAL(&supervisorMux);
  }
  if (action != OTA_BOOT_ACTION_ROLLBACK) return;

  portENTER_CRITICAL(&supervisorMux);
  supervisor.phase = OTA_BOOT_PHASE_HALTED;
  portEXIT_CRITICAL(&supervisorMux);
  performRollback(rollbackReason);
}

bool otaBootHealthSupervising() {
  portENTER_CRITICAL(&supervisorMux);
  OtaBootHealthPhase phase = supervisor.phase;
  portEXIT_CRITICAL(&supervisorMux);
  return phase == OTA_BOOT_PHASE_MONITORING ||
    phase == OTA_BOOT_PHASE_MARK_VALID || phase == OTA_BOOT_PHASE_ROLLBACK;
}

const char* otaBootHealthStatusLabel() {
  portENTER_CRITICAL(&supervisorMux);
  OtaBootHealthPhase phase = supervisor.phase;
  portEXIT_CRITICAL(&supervisorMux);
  return otaBootHealthPhaseLabel(phase);
}

const char* otaBootHealthLastRollbackReason() {
  return otaBootHealthReasonLabel(observedRollbackReason);
}

void otaBootHealthLogStatus() {
  portENTER_CRITICAL(&supervisorMux);
  uint8_t readyBits = supervisor.readyBits;
  portEXIT_CRITICAL(&supervisorMux);
  Serial.printf(
    "[ota-boot] state=%s ready=0x%02x rollback=%s query=0x%x\n",
    otaBootHealthStatusLabel(), readyBits,
    otaBootHealthLastRollbackReason(), lastStateQueryError);
}
