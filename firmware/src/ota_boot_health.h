#pragma once

#include <Arduino.h>

#include "ota_boot_health_logic.h"

// Call before any fallible device/application initialisation in setup().
void otaBootHealthArmEarly(uint32_t nowMs);

// Concrete subsystem signals. All are no-ops for ordinary/non-pending boots.
void otaBootHealthReady(OtaBootReadyBit bit);
void otaBootHealthCriticalFailure(OtaBootHealthReason reason);

// Poll at the first and last safe points of normal execution. A rollback path
// never returns. Mark-valid is emitted exactly once by the pure supervisor.
void otaBootHealthPoll(uint32_t nowMs);

bool otaBootHealthSupervising();
const char* otaBootHealthStatusLabel();
const char* otaBootHealthLastRollbackReason();
int32_t otaBootHealthLastRollbackError();
void otaBootHealthLogStatus();
