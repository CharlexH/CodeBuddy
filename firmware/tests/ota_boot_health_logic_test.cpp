#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_boot_health_logic.h"

static void expect(bool value, const char* message) {
  if (!value) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static void signalAll(OtaBootHealthState* state) {
  otaBootHealthSignal(state, OTA_BOOT_READY_DISPLAY);
  otaBootHealthSignal(state, OTA_BOOT_READY_STORAGE);
  otaBootHealthSignal(state, OTA_BOOT_READY_BUTTONS);
  otaBootHealthSignal(state, OTA_BOOT_READY_BLE);
  otaBootHealthSignal(state, OTA_BOOT_READY_EVENT_LOOP);
}

struct FakeAdapter {
  char events[256] = "";
  bool markValidSucceeds = true;

  void event(const char* value) {
    if (events[0]) strncat(events, ",", sizeof(events) - strlen(events) - 1);
    strncat(events, value, sizeof(events) - strlen(events) - 1);
  }

  bool markValid() {
    event("mark-valid");
    return markValidSucceeds;
  }
  void persistReason(OtaBootHealthReason reason) {
    char value[32];
    snprintf(value, sizeof(value), "reason:%s", otaBootHealthReasonLabel(reason));
    event(value);
  }
  int markInvalidRollbackAndReboot() {
    event("rollback");
    return -1;  // The real API returns only when rollback/reboot failed.
  }
  void restart() { event("restart"); }
  void fatalStop() { event("abort"); }
};

static void testNonPendingAndUnknownAreInert() {
  OtaBootHealthState ordinary = otaBootHealthInitial();
  otaBootHealthArm(&ordinary, OTA_BOOT_QUERY_NON_PENDING, 100, false);
  signalAll(&ordinary);
  FakeAdapter adapter;
  otaBootHealthRun(&ordinary, 1000, &adapter);
  expect(ordinary.phase == OTA_BOOT_PHASE_INERT,
         "ordinary VALID/UNDEFINED/USB boots must remain inert");
  expect(adapter.events[0] == 0,
         "ordinary boots must never call rollback APIs");

  OtaBootHealthState unknown = otaBootHealthInitial();
  otaBootHealthArm(&unknown, OTA_BOOT_QUERY_UNKNOWN, 100, true);
  signalAll(&unknown);
  otaBootHealthRun(&unknown, 40000, &adapter);
  expect(unknown.phase == OTA_BOOT_PHASE_QUERY_UNKNOWN,
         "state-query errors must stay explicitly unknown");
  expect(adapter.events[0] == 0,
         "unknown state must neither mark nor brick a possible USB boot");
}

static void testEveryReadinessBitIsRequired() {
  const OtaBootReadyBit bits[] = {
    OTA_BOOT_READY_DISPLAY,
    OTA_BOOT_READY_STORAGE,
    OTA_BOOT_READY_BUTTONS,
    OTA_BOOT_READY_BLE,
    OTA_BOOT_READY_EVENT_LOOP,
  };
  for (size_t missing = 0; missing < sizeof(bits) / sizeof(bits[0]); ++missing) {
    OtaBootHealthState state = otaBootHealthInitial();
    otaBootHealthArm(&state, OTA_BOOT_QUERY_PENDING_VERIFY, 100, true);
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); ++i) {
      if (i != missing) otaBootHealthSignal(&state, bits[i]);
    }
    FakeAdapter adapter;
    otaBootHealthRun(&state, 101, &adapter);
    expect(state.phase == OTA_BOOT_PHASE_MONITORING,
           "a pending boot must wait when any concrete readiness bit is missing");
    expect(adapter.events[0] == 0,
           "incomplete readiness must not mark the image valid early");
    otaBootHealthRun(&state, 100 + OTA_BOOT_HEALTH_TIMEOUT_MS, &adapter);
    expect(state.phase == OTA_BOOT_PHASE_HALTED &&
             strstr(adapter.events, "reason:timeout,rollback") == adapter.events,
           "every missing readiness bit must end in active timeout rollback");
  }
}

static void testSuccessBeforeDeadlineMarksExactlyOnce() {
  OtaBootHealthState state = otaBootHealthInitial();
  otaBootHealthArm(&state, OTA_BOOT_QUERY_PENDING_VERIFY, 100, true);
  signalAll(&state);
  FakeAdapter adapter;
  otaBootHealthRun(&state, 100 + OTA_BOOT_HEALTH_TIMEOUT_MS - 1, &adapter);
  expect(state.phase == OTA_BOOT_PHASE_VALID,
         "all health signals before the deadline must validate the image");
  expect(strcmp(adapter.events, "mark-valid") == 0,
         "mark-valid must be the only successful health API call");
  otaBootHealthRun(&state, 100 + OTA_BOOT_HEALTH_TIMEOUT_MS, &adapter);
  expect(strcmp(adapter.events, "mark-valid") == 0,
         "repeated polls must never mark valid twice");
}

static void testDeadlineExactAndWrapSafe() {
  OtaBootHealthState exact = otaBootHealthInitial();
  otaBootHealthArm(&exact, OTA_BOOT_QUERY_PENDING_VERIFY, 100, true);
  signalAll(&exact);
  FakeAdapter adapter;
  otaBootHealthRun(&exact, 100 + OTA_BOOT_HEALTH_TIMEOUT_MS, &adapter);
  expect(exact.phase == OTA_BOOT_PHASE_HALTED,
         "the exact deadline must time out even if readiness was not polled earlier");
  expect(strstr(adapter.events, "reason:timeout,rollback") == adapter.events,
         "deadline timeout must actively request rollback before fallback restart");

  OtaBootHealthState wrapped = otaBootHealthInitial();
  uint32_t start = UINT32_MAX - 20;
  otaBootHealthArm(&wrapped, OTA_BOOT_QUERY_PENDING_VERIFY, start, true);
  FakeAdapter wrappedAdapter;
  otaBootHealthRun(&wrapped, start + OTA_BOOT_HEALTH_TIMEOUT_MS - 1,
                   &wrappedAdapter);
  expect(wrapped.phase == OTA_BOOT_PHASE_MONITORING,
         "wrapped deadline must remain active one millisecond before expiry");
  otaBootHealthRun(&wrapped, start + OTA_BOOT_HEALTH_TIMEOUT_MS,
                   &wrappedAdapter);
  expect(wrapped.phase == OTA_BOOT_PHASE_HALTED,
         "wrapped deadline must expire exactly at its target");
}

static void testExplicitFailureAndInvalidLayoutRollbackOnce() {
  OtaBootHealthState state = otaBootHealthInitial();
  otaBootHealthArm(&state, OTA_BOOT_QUERY_PENDING_VERIFY, 0, true);
  otaBootHealthFail(&state, OTA_BOOT_REASON_STORAGE);
  FakeAdapter adapter;
  otaBootHealthRun(&state, 1, &adapter);
  expect(strstr(adapter.events, "reason:storage,rollback") == adapter.events,
         "an explicit critical init failure must immediately roll back");
  char once[sizeof(adapter.events)];
  strcpy(once, adapter.events);
  otaBootHealthRun(&state, 2, &adapter);
  expect(strcmp(adapter.events, once) == 0,
         "a latched rollback must never call its API twice");

  OtaBootHealthState layout = otaBootHealthInitial();
  otaBootHealthArm(&layout, OTA_BOOT_QUERY_PENDING_VERIFY, 0, false);
  FakeAdapter layoutAdapter;
  otaBootHealthRun(&layout, 0, &layoutAdapter);
  expect(strstr(layoutAdapter.events, "reason:layout,rollback") ==
           layoutAdapter.events,
         "a pending image without the exact OTA layout must fail closed");
}

static void testMarkValidFailureFallsThroughToRollback() {
  OtaBootHealthState state = otaBootHealthInitial();
  otaBootHealthArm(&state, OTA_BOOT_QUERY_PENDING_VERIFY, 0, true);
  signalAll(&state);
  FakeAdapter adapter;
  adapter.markValidSucceeds = false;
  otaBootHealthRun(&state, 1, &adapter);
  expect(strcmp(adapter.events,
    "mark-valid,reason:mark-valid,rollback,restart,restart,restart,abort") == 0,
    "mark-valid errors must roll back, then use bounded restart/abort fallback");
  expect(state.phase == OTA_BOOT_PHASE_HALTED,
         "a returned rollback API must irreversibly halt the supervisor");
}

static void testRollbackReturnUsesBoundedFallbackAndNeverResumes() {
  OtaBootHealthState state = otaBootHealthInitial();
  otaBootHealthArm(&state, OTA_BOOT_QUERY_PENDING_VERIFY, 0, true);
  otaBootHealthFail(&state, OTA_BOOT_REASON_BLE);
  FakeAdapter adapter;
  otaBootHealthRun(&state, 1, &adapter);
  expect(strcmp(adapter.events,
    "reason:ble,rollback,restart,restart,restart,abort") == 0,
    "rollback API return must trigger exactly three restart attempts then abort");
  otaBootHealthSignal(&state, OTA_BOOT_READY_EVENT_LOOP);
  otaBootHealthRun(&state, 2, &adapter);
  expect(strcmp(adapter.events,
    "reason:ble,rollback,restart,restart,restart,abort") == 0,
    "halted supervisor must never resume or validate later");

  OtaBootHealthState prior = otaBootHealthInitial();
  otaBootHealthArm(&prior, OTA_BOOT_QUERY_NON_PENDING, 0, true);
  FakeAdapter priorAdapter;
  otaBootHealthRun(&prior, OTA_BOOT_HEALTH_TIMEOUT_MS, &priorAdapter);
  expect(prior.phase == OTA_BOOT_PHASE_INERT && priorAdapter.events[0] == 0,
         "the prior valid slot after rollback must boot without a rollback loop");
}

int main() {
  testNonPendingAndUnknownAreInert();
  testEveryReadinessBitIsRequired();
  testSuccessBeforeDeadlineMarksExactlyOnce();
  testDeadlineExactAndWrapSafe();
  testExplicitFailureAndInvalidLayoutRollbackOnce();
  testMarkValidFailureFallsThroughToRollback();
  testRollbackReturnUsesBoundedFallbackAndNeverResumes();
  return 0;
}
