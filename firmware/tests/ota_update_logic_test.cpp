#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_update_logic.h"

static void expect(bool value, const char* message) {
  if (!value) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static OtaUpdateInputs ready() {
  OtaUpdateInputs in = {};
  in.nowMs = 1000;
  in.offerPending = true;
  in.bleConnected = true;
  in.wifiProvisioned = true;
  in.wifiOnline = true;
  in.trustedTime = true;
  in.offerFresh = true;
  in.externalPower = true;
  in.batteryKnown = true;
  in.batteryPercent = 100;
  return in;
}

static void testReadinessAndPowerGates() {
  OtaUpdateInputs in = ready();
  expect(otaUpdateGate(in, true) == OTA_GATE_READY,
         "fully ready inputs should pass");
  in.externalPower = false;
  in.batteryPercent = 49;
  expect(otaUpdateGate(in, true) == OTA_GATE_POWER,
         "49 percent without external power must fail");
  in.batteryPercent = 50;
  expect(otaUpdateGate(in, true) == OTA_GATE_READY,
         "50 percent is the inclusive battery threshold");
  in.batteryKnown = false;
  expect(otaUpdateGate(in, true) == OTA_GATE_POWER,
         "unknown battery without external power must fail closed");
  in.externalPower = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_READY,
         "external power permits an unknown battery reading");

  in.prompt = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_CONFLICT,
         "approval prompt must win over OTA");
  in.prompt = false;
  in.transfer = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_CONFLICT,
         "character transfer conflicts with OTA");
  in.transfer = false;
  in.provisioning = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_CONFLICT,
         "Wi-Fi provisioning conflicts with OTA");
  in.provisioning = false;
  in.passkey = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_CONFLICT,
         "BLE passkey UI conflicts with OTA");
  in.passkey = false;
  in.functional = true;
  expect(otaUpdateGate(in, true) == OTA_GATE_CONFLICT,
         "unrelated functional UI conflicts with OTA");
}

static void testCoordinationEndsOnlyAfterAuthentication() {
  OtaUpdateInputs in = ready();
  in.bleConnected = false;
  in.offerFresh = false;
  in.offerPending = false;
  expect(otaUpdateGate(in, true) == OTA_GATE_NO_OFFER,
         "pre-authentication requires a live accepted offer");
  expect(otaUpdateGate(in, false) == OTA_GATE_READY,
         "signed descriptor no longer depends on the BLE hint lifetime");

  in.wifiOnline = false;
  expect(otaUpdateGate(in, false) == OTA_GATE_WIFI,
         "authenticated transfer still requires live Wi-Fi");
  in.wifiOnline = true;
  in.trustedTime = false;
  expect(otaUpdateGate(in, false) == OTA_GATE_TIME,
         "authenticated transfer still requires trusted time");
}

static void testBoundedReadinessWaitAndWrap() {
  OtaUpdateInputs in = ready();
  in.wifiOnline = false;
  OtaUpdateMachine m = otaUpdateMachineInitial();
  otaUpdateStep(&m, in, true, false);
  expect(m.phase == OTA_PHASE_WAIT_READY,
         "physical A may enter a bounded Wi-Fi readiness wait");
  in.nowMs += OTA_UPDATE_READY_TIMEOUT_MS - 1;
  otaUpdateStep(&m, in, false, false);
  expect(m.phase == OTA_PHASE_WAIT_READY,
         "readiness wait remains active before its deadline");
  in.nowMs += 2;
  otaUpdateStep(&m, in, false, false);
  expect(m.phase == OTA_PHASE_ERROR,
         "readiness timeout must terminate without network access");

  expect(!otaDeadlineExpired(UINT32_MAX - 5, 10),
         "wrapped deadline remains active");
  expect(otaDeadlineExpired(11, 10),
         "wrapped deadline expires after its target");
}

static void testTargetAndHttpValidation() {
  OtaPartitionInfo running = {
    1, 0x10000, OTA_SLOT_CAPACITY_BYTES, OTA_PARTITION_APP,
    OTA_PARTITION_SUBTYPE_APP_OTA_MIN_VALUE, false,
  };
  OtaPartitionInfo target = {
    2, 0x340000, OTA_SLOT_CAPACITY_BYTES, OTA_PARTITION_APP,
    OTA_PARTITION_SUBTYPE_APP_OTA_MIN_VALUE + 1, false,
  };
  expect(otaTargetValid(&running, &target, 1000),
         "different inactive OTA slot should pass");
  expect(!otaTargetValid(nullptr, &target, 1000),
         "missing running partition must fail");
  expect(!otaTargetValid(&running, nullptr, 1000),
         "missing target partition must fail");
  target.identity = running.identity;
  expect(!otaTargetValid(&running, &target, 1000),
         "same partition identity must fail");
  target.identity = 2;
  target.address = running.address;
  expect(!otaTargetValid(&running, &target, 1000),
         "same partition address must fail");
  target.address = 0x340000;
  target.type = OTA_PARTITION_DATA;
  expect(!otaTargetValid(&running, &target, 1000),
         "data partition must fail");
  target.type = OTA_PARTITION_APP;
  target.subtype = OTA_PARTITION_SUBTYPE_APP_OTA_MAX_EXCLUSIVE;
  expect(!otaTargetValid(&running, &target, 1000),
         "exclusive OTA subtype maximum must fail");
  target.subtype = OTA_PARTITION_SUBTYPE_APP_OTA_MIN_VALUE + 1;
  target.size = 999;
  expect(!otaTargetValid(&running, &target, 1000),
         "undersized inactive slot must fail");
  target.size = OTA_SLOT_CAPACITY_BYTES;
  running.pendingVerify = true;
  expect(!otaTargetValid(&running, &target, 1000),
         "pending-verify running image must fail closed");

  OtaHttpMeta response = {200, 1000, false, true};
  expect(otaHttpMetaExactValid(response, 1000, OTA_SLOT_CAPACITY_BYTES),
         "strict exact firmware response should pass");
  response.status = 302;
  expect(!otaHttpMetaExactValid(response, 1000, OTA_SLOT_CAPACITY_BYTES),
         "redirect response must fail");
  response.status = 200;
  response.redirected = true;
  expect(!otaHttpMetaExactValid(response, 1000, OTA_SLOT_CAPACITY_BYTES),
         "followed redirect must also fail");
  response.redirected = false;
  response.identityEncoding = false;
  expect(!otaHttpMetaExactValid(response, 1000, OTA_SLOT_CAPACITY_BYTES),
         "encoded response must fail");
  response.identityEncoding = true;
  response.contentLength = 999;
  expect(!otaHttpMetaExactValid(response, 1000, OTA_SLOT_CAPACITY_BYTES),
         "firmware Content-Length mismatch must fail");

  response.contentLength = OTA_MANIFEST_MAX_BYTES;
  expect(otaHttpMetaBoundedValid(response, OTA_MANIFEST_MAX_BYTES),
         "bounded manifest response at the maximum should pass");
  response.contentLength = OTA_MANIFEST_MAX_BYTES + 1;
  expect(!otaHttpMetaBoundedValid(response, OTA_MANIFEST_MAX_BYTES),
         "oversized manifest response must fail before reading its body");
  response.contentLength = -1;
  expect(!otaHttpMetaBoundedValid(response, OTA_MANIFEST_MAX_BYTES),
         "missing Content-Length must fail");
}

static void testDigestProgressAndUiHelpers() {
  uint8_t left[32] = {}, right[32] = {}, decoded[32] = {};
  expect(otaDecodeSha256(
           "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
           decoded),
         "canonical lower-case SHA-256 should decode");
  expect(decoded[0] == 0 && decoded[31] == 31,
         "decoded digest must preserve byte order");
  expect(!otaDecodeSha256(
           "GG0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
           decoded),
         "non-hex digest must fail");
  expect(otaConstantTimeEqual(left, right, sizeof(left)),
         "equal digests should pass");
  right[0] = 1;
  expect(!otaConstantTimeEqual(left, right, sizeof(left)),
         "first-byte mismatch must fail");
  right[0] = 0;
  right[31] = 1;
  expect(!otaConstantTimeEqual(left, right, sizeof(left)),
         "last-byte mismatch must fail");
  expect(otaProgressPercent(UINT64_C(0xffffffff), UINT64_C(0xffffffff)) == 100,
         "64-bit progress must not overflow");
  expect(otaProgressPercent(1, 0) == 0,
         "zero total must report zero percent");
  expect(otaUpdateOverallProgress(OTA_PHASE_DOWNLOAD, 50, 0, 100) == 40,
         "download should occupy the first eighty percent");
  expect(otaUpdateOverallProgress(OTA_PHASE_READBACK, 100, 50, 100) == 90,
         "readback should occupy the final twenty percent");
  expect(otaUpdateOverallProgress(OTA_PHASE_DIGEST, 100, 100, 100) == 100,
         "digest phase should remain at one hundred percent");
  expect(otaUiSurface(true, true) == OTA_UI_APPROVAL,
         "approval always wins UI priority");
  expect(otaUiSurface(false, true) == OTA_UI_UPDATE,
         "OTA surface is visible without approval");
}

static void testHttpBodyPollingDeadlinesAndExactness() {
  OtaHttpBodyState body = otaHttpBodyState(1000, 100, 5000);
  OtaHttpBodyPoll wait = otaHttpBodyPoll(body, 1001, 0, true, 256);
  expect(wait.decision == OTA_HTTP_BODY_WAIT,
         "connected body with no bytes should wait without blocking");
  OtaHttpBodyPoll first = otaHttpBodyPoll(body, 1002, 80, true, 64);
  expect(first.decision == OTA_HTTP_BODY_READ && first.bytes == 64,
         "a poll may request at most one bounded network chunk");
  expect(otaHttpBodyCommit(&body, 1002, first.bytes),
         "actual network bytes should advance exact accounting");
  OtaHttpBodyPoll final = otaHttpBodyPoll(body, 1003, 36, true, 64);
  expect(final.decision == OTA_HTTP_BODY_READ && final.bytes == 36,
         "last request must be clipped to the signed body length");
  expect(otaHttpBodyCommit(&body, 1003, final.bytes),
         "last body chunk should commit");
  expect(otaHttpBodyPoll(body, 1004, 0, false, 64).decision ==
           OTA_HTTP_BODY_COMPLETE,
         "exact body completion should not require a live socket");
  expect(otaHttpBodyPoll(body, 1004, 1, true, 64).decision ==
           OTA_HTTP_BODY_FAILURE,
         "a byte beyond Content-Length must fail closed");

  OtaHttpBodyState idle = otaHttpBodyState(2000, 10, 5000);
  expect(otaHttpBodyPoll(idle, 7000, 0, true, 10).decision ==
           OTA_HTTP_BODY_FAILURE,
         "five-second body idle timeout must fail");
  OtaHttpBodyState absolute = otaHttpBodyState(3000, 10, 5000);
  expect(otaHttpBodyPoll(
           absolute, 8001, 10, true, 10
         ).decision == OTA_HTTP_BODY_FAILURE,
         "absolute resource timeout must win even when bytes arrive");
  OtaHttpBodyState dropped = otaHttpBodyState(9000, 10, 5000);
  expect(otaHttpBodyPoll(dropped, 9001, 0, false, 10).decision ==
           OTA_HTTP_BODY_FAILURE,
         "interrupted body must fail without retry");
  expect(!otaHttpBodyCommit(&dropped, 9001, 11),
         "body accounting may never exceed Content-Length");
}

struct Fake {
  OtaUpdateEvent events[256];
  size_t eventCount;
  OtaUpdateEvent failEvent;
  bool failEnabled;
  bool digestMatches;
  bool pendingManifestOnce;
  bool pendingFirmwareOnce;
  bool manifestPendingReturned;
  bool firmwarePendingReturned;
  uint32_t firmwareReadSizes[8];
  size_t firmwareReadCount;
  size_t firmwareReadIndex;
  uint64_t imageSize;
};

static OtaUpdateActionResult fakeAction(
  void* opaque,
  OtaUpdateEvent event,
  uint64_t offset,
  uint32_t maximumBytes
) {
  Fake* fake = static_cast<Fake*>(opaque);
  fake->events[fake->eventCount++] = event;
  if (fake->failEnabled && event == fake->failEvent)
    return otaActionFailure();
  if (event == OTA_EVENT_DIGEST_MATCH)
    return fake->digestMatches ? otaActionComplete() : otaActionFailure();
  if (event == OTA_EVENT_READ_MANIFEST) {
    if (fake->pendingManifestOnce && !fake->manifestPendingReturned) {
      fake->manifestPendingReturned = true;
      return otaActionPending();
    }
    return otaActionComplete(64);
  }
  if (event == OTA_EVENT_READ_SIGNATURE) return otaActionComplete(70);
  if (event == OTA_EVENT_DOWNLOAD) {
    if (fake->pendingFirmwareOnce && !fake->firmwarePendingReturned) {
      fake->firmwarePendingReturned = true;
      return otaActionPending();
    }
    uint32_t amount = maximumBytes;
    if (fake->firmwareReadIndex < fake->firmwareReadCount)
      amount = fake->firmwareReadSizes[fake->firmwareReadIndex++];
    bool final = offset + amount == fake->imageSize;
    return final ? otaActionComplete(amount) : otaActionProgress(amount);
  }
  if (event == OTA_EVENT_READBACK)
    return otaActionComplete(maximumBytes);
  return otaActionComplete();
}

static OtaUpdateMachine machine(Fake* fake, uint64_t imageSize = 8192) {
  OtaUpdateMachine result = otaUpdateMachineInitial();
  result.action = fakeAction;
  result.actionContext = fake;
  result.imageSize = imageSize;
  result.chunkSize = OTA_UPDATE_CHUNK_BYTES;
  fake->imageSize = imageSize;
  return result;
}

static void tickUntilTerminal(
  OtaUpdateMachine* machine,
  OtaUpdateInputs inputs,
  bool confirm = true
) {
  for (int i = 0; i < 100 && !otaUpdateTerminal(*machine); ++i)
    otaUpdateStep(machine, inputs, confirm && i == 0, false);
}

static bool saw(const Fake& fake, OtaUpdateEvent event) {
  for (size_t i = 0; i < fake.eventCount; ++i)
    if (fake.events[i] == event) return true;
  return false;
}

static size_t count(const Fake& fake, OtaUpdateEvent event) {
  size_t result = 0;
  for (size_t i = 0; i < fake.eventCount; ++i)
    if (fake.events[i] == event) ++result;
  return result;
}

static void testNoActionBeforePhysicalConfirmAndSuccessOrder() {
  Fake fake = {};
  fake.digestMatches = true;
  OtaUpdateMachine update = machine(&fake);
  OtaUpdateInputs inputs = ready();
  otaUpdateStep(&update, inputs, false, false);
  expect(fake.eventCount == 0 && update.phase == OTA_PHASE_CONFIRM,
         "no network or flash action may run before physical A");
  tickUntilTerminal(&update, inputs);

  const OtaUpdateEvent expected[] = {
    OTA_EVENT_OPEN_MANIFEST,
    OTA_EVENT_READ_MANIFEST,
    OTA_EVENT_OPEN_SIGNATURE,
    OTA_EVENT_READ_SIGNATURE,
    OTA_EVENT_AUTHENTICATE,
    OTA_EVENT_SELECT,
    OTA_EVENT_OPEN_FIRMWARE,
    OTA_EVENT_BEGIN,
    OTA_EVENT_DOWNLOAD,
    OTA_EVENT_WRITE,
    OTA_EVENT_DOWNLOAD,
    OTA_EVENT_WRITE,
    OTA_EVENT_FINISH_DOWNLOAD,
    OTA_EVENT_END,
    OTA_EVENT_READBACK,
    OTA_EVENT_READBACK,
    OTA_EVENT_DIGEST_MATCH,
    OTA_EVENT_FINAL_GATE,
    OTA_EVENT_SET_BOOT,
    OTA_EVENT_RESTART,
  };
  expect(fake.eventCount == sizeof(expected) / sizeof(expected[0]),
         "successful OTA should have the exact bounded event count");
  expect(memcmp(fake.events, expected, sizeof(expected)) == 0,
         "successful OTA event order must be strict");
  expect(update.phase == OTA_PHASE_RESTARTING,
         "successful OTA should reach restart");
  expect(update.receivedBytes == update.imageSize &&
           update.readbackBytes == update.imageSize,
         "successful OTA must write and read back the exact signed size");
}

static void testPendingAndPartialNetworkReadsStayBounded() {
  Fake fake = {};
  fake.digestMatches = true;
  fake.pendingManifestOnce = true;
  fake.pendingFirmwareOnce = true;
  fake.firmwareReadSizes[0] = 1024;
  fake.firmwareReadSizes[1] = 3072;
  fake.firmwareReadSizes[2] = 4096;
  fake.firmwareReadCount = 3;
  OtaUpdateMachine update = machine(&fake);
  tickUntilTerminal(&update, ready());
  expect(update.phase == OTA_PHASE_RESTARTING,
         "pending and partial reads should resume successfully");
  expect(count(fake, OTA_EVENT_READ_MANIFEST) == 2,
         "manifest pending result consumes one poll and no other action");
  expect(count(fake, OTA_EVENT_DOWNLOAD) == 4,
         "firmware pending/partial reads each consume one poll");
  expect(count(fake, OTA_EVENT_WRITE) == 3,
         "every non-empty network chunk is written on a later poll");
}

static void testFailureCleanupAndNoBootSwitch() {
  const OtaUpdateEvent failures[] = {
    OTA_EVENT_OPEN_MANIFEST,
    OTA_EVENT_READ_MANIFEST,
    OTA_EVENT_OPEN_SIGNATURE,
    OTA_EVENT_READ_SIGNATURE,
    OTA_EVENT_AUTHENTICATE,
    OTA_EVENT_SELECT,
    OTA_EVENT_OPEN_FIRMWARE,
    OTA_EVENT_BEGIN,
    OTA_EVENT_DOWNLOAD,
    OTA_EVENT_WRITE,
    OTA_EVENT_FINISH_DOWNLOAD,
    OTA_EVENT_END,
    OTA_EVENT_READBACK,
    OTA_EVENT_DIGEST_MATCH,
    OTA_EVENT_FINAL_GATE,
    OTA_EVENT_SET_BOOT,
  };
  for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
    Fake fake = {};
    fake.digestMatches = true;
    fake.failEnabled = true;
    fake.failEvent = failures[i];
    OtaUpdateMachine update = machine(&fake);
    tickUntilTerminal(&update, ready());
    expect(update.phase == OTA_PHASE_ERROR,
           "every injected operation failure must terminate in error");
    bool beginsHandle = failures[i] > OTA_EVENT_BEGIN;
    bool beforeEnd = failures[i] < OTA_EVENT_END;
    size_t expectedAborts = beginsHandle && beforeEnd ? 1 : 0;
    expect(count(fake, OTA_EVENT_ABORT) == expectedAborts,
           "active OTA handle must abort exactly once only before end");
    if (failures[i] != OTA_EVENT_SET_BOOT)
      expect(!saw(fake, OTA_EVENT_SET_BOOT),
             "failure before set_boot must never attempt a boot switch");
    expect(!saw(fake, OTA_EVENT_RESTART),
           "failure must never restart into an unverified image");
  }
}

static void testCancellationAndApprovalConflict() {
  OtaUpdateInputs inputs = ready();
  Fake before = {};
  before.digestMatches = true;
  OtaUpdateMachine first = machine(&before);
  otaUpdateStep(&first, inputs, true, false);
  otaUpdateStep(&first, inputs, false, true);
  expect(first.phase == OTA_PHASE_CANCELLED &&
           !saw(before, OTA_EVENT_ABORT),
         "pre-begin cancellation needs no abort");

  Fake active = {};
  active.digestMatches = true;
  OtaUpdateMachine second = machine(&active);
  for (int i = 0; i < 10 && second.phase != OTA_PHASE_DOWNLOAD; ++i)
    otaUpdateStep(&second, inputs, i == 0, false);
  otaUpdateStep(&second, inputs, false, true);
  expect(second.phase == OTA_PHASE_CANCELLED &&
           count(active, OTA_EVENT_ABORT) == 1,
         "active-handle cancellation must abort exactly once");

  Fake prompt = {};
  prompt.digestMatches = true;
  OtaUpdateMachine third = machine(&prompt);
  for (int i = 0; i < 10 && third.phase != OTA_PHASE_DOWNLOAD; ++i)
    otaUpdateStep(&third, inputs, i == 0, false);
  inputs.prompt = true;
  otaUpdateStep(&third, inputs, false, false);
  expect(third.phase == OTA_PHASE_CANCELLED &&
           count(prompt, OTA_EVENT_ABORT) == 1 &&
           !saw(prompt, OTA_EVENT_SET_BOOT),
         "approval conflict must cancel OTA at the next safe poll");
}

static void testMalformedActionAccountingFailsClosed() {
  Fake zero = {};
  zero.digestMatches = true;
  zero.firmwareReadSizes[0] = 0;
  zero.firmwareReadCount = 1;
  OtaUpdateMachine first = machine(&zero);
  tickUntilTerminal(&first, ready());
  expect(first.phase == OTA_PHASE_ERROR,
         "zero-byte progress must fail rather than spin");

  Fake oversized = {};
  oversized.digestMatches = true;
  oversized.firmwareReadSizes[0] = OTA_UPDATE_CHUNK_BYTES + 1;
  oversized.firmwareReadCount = 1;
  OtaUpdateMachine second = machine(&oversized);
  tickUntilTerminal(&second, ready());
  expect(second.phase == OTA_PHASE_ERROR,
         "adapter may not report more bytes than requested");
}

int main() {
  testReadinessAndPowerGates();
  testCoordinationEndsOnlyAfterAuthentication();
  testBoundedReadinessWaitAndWrap();
  testTargetAndHttpValidation();
  testDigestProgressAndUiHelpers();
  testHttpBodyPollingDeadlinesAndExactness();
  testNoActionBeforePhysicalConfirmAndSuccessOrder();
  testPendingAndPartialNetworkReadsStayBounded();
  testFailureCleanupAndNoBootSwitch();
  testCancellationAndApprovalConflict();
  testMalformedActionAccountingFailsClosed();
  puts("ota update logic tests passed");
  return 0;
}
