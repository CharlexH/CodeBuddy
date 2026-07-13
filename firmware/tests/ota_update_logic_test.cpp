#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_update_logic.h"
#include "ota_policy_logic.h"

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

static void testSignedOfferPolicy() {
  OtaOfferPolicy none = otaOfferPolicy(false, false, false);
  expect(!none.wake && !none.automatic && !none.requiresConfirmation,
         "no pending offer must not open an OTA surface");

  OtaOfferPolicy legacyAsk = otaOfferPolicy(true, false, true);
  expect(!legacyAsk.wake && !legacyAsk.automatic &&
           !legacyAsk.requiresConfirmation,
         "legacy unsigned offers must never gain Direct behavior");

  OtaOfferPolicy signedAsk = otaOfferPolicy(true, true, false);
  expect(signedAsk.wake && !signedAsk.automatic &&
           signedAsk.requiresConfirmation,
         "verified signed Ask offers should wake directly to A/B");

  OtaOfferPolicy signedDirect = otaOfferPolicy(true, true, true);
  expect(signedDirect.wake && signedDirect.automatic &&
           !signedDirect.requiresConfirmation,
         "verified signed Direct offers should wake and auto-authorize");
}

static void testDirectOfferStartsAtomically() {
  OtaPollStartPlan direct = otaPollStartPlan(false, true, true, true);
  expect(direct.arm && direct.continueSamePoll,
         "new verified Direct offer must continue in its arrival poll");

  OtaOfferState shared = {};
  shared.windowOpen = true;
  shared.pending = true;
  shared.signedAuthorized = true;
  shared.windowDeadlineMs = 10000;
  shared.offerDeadlineMs = 9000;
  OtaOfferState privateOffer = {};
  expect(otaOfferExecutionAllowed(
           &shared, 1000, true, false, false, false, true, 82) &&
         otaOfferConsume(&shared, &privateOffer),
         "Direct arrival poll should atomically move the verified offer private");

  OtaUpdateMachine machine = otaUpdateMachineInitial();
  OtaUpdateInputs in = ready();
  in.offerPending = privateOffer.pending;
  in.offerFresh = otaOfferWindowActive(privateOffer, in.nowMs) &&
    otaOfferDeadlineActive(in.nowMs, privateOffer.offerDeadlineMs);
  otaUpdateStep(&machine, in, true, false);
  expect(machine.phase == OTA_PHASE_OPEN_MANIFEST,
         "atomic Direct confirmation should leave CONFIRM in frame one");

  otaOfferLifecyclePoll(&shared, 1001, false, false, false, false);
  expect(privateOffer.pending && machine.phase == OTA_PHASE_OPEN_MANIFEST,
         "later shared-offer disconnect must not cancel the private transaction");

  OtaPollStartPlan ask = otaPollStartPlan(false, true, true, false);
  expect(ask.arm && !ask.continueSamePoll,
         "Ask must remain live until physical A on a later poll");
  OtaPollStartPlan legacy = otaPollStartPlan(false, true, false, true);
  expect(legacy.arm && !legacy.continueSamePoll,
         "legacy unsigned offers must never continue as Direct");

  OtaOfferState blocked = privateOffer;
  expect(!otaOfferExecutionAllowed(
           &blocked, 1000, true, true, false, false, true, 82) &&
         !blocked.pending,
         "Direct still fails closed on approval conflicts");
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
  expect(!otaHttpIoExpired(4999, 5000, 6000),
         "async open remains pending before both hard deadlines");
  expect(otaHttpIoExpired(5000, 5000, 6000),
         "async open hard timeout expires without another network call");
  expect(otaHttpIoExpired(5500, 7000, 5500),
         "async open idle timeout also fails closed");
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
  expect(otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_OK, OTA_RUNNING_IMAGE_VALID, false),
         "explicit valid running state should allow OTA");
  expect(!otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_OK, OTA_RUNNING_IMAGE_PENDING_VERIFY, true),
         "pending-verify state must fail even when initial layout is trusted");
  expect(!otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_OK, OTA_RUNNING_IMAGE_OTHER, true),
         "unexpected recorded image state must fail closed");
  expect(otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_NOT_FOUND, OTA_RUNNING_IMAGE_OTHER, true),
         "documented initial no-record state may pass only after layout verification");
  expect(!otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_NOT_FOUND, OTA_RUNNING_IMAGE_OTHER, false),
         "unverified no-record state must fail closed");
  expect(!otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_ERROR, OTA_RUNNING_IMAGE_VALID, true),
         "unexpected state-query errors must fail closed");

  // Exact first esp_ota_select_entry_t from Arduino-ESP32 boot_app0.bin:
  // ota_seq=1, 20-byte label erased, ota_state=0xffffffff, crc=0x4743989a.
  const uint8_t bootApp0Entry[32] = {
    0x01, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff,
    0x9a, 0x98, 0x43, 0x47,
  };
  uint32_t otaSequence = otaReadLittleEndianU32(bootApp0Entry);
  uint32_t otaState = otaReadLittleEndianU32(bootApp0Entry + 24);
  expect(otaSequence == 1 && otaState == OTA_IMAGE_STATE_UNDEFINED_RAW,
         "boot_app0 fixture must retain ota_seq=1 and erased undefined state");
  OtaRunningImageState bootstrapState = otaRunningImageStateFromRaw(otaState);
  expect(bootstrapState == OTA_RUNNING_IMAGE_UNDEFINED &&
           otaRunningStateAllowsUpdate(
             OTA_STATE_QUERY_OK, bootstrapState, true),
         "verified USB bootstrap undefined state must allow the first wireless OTA");
  expect(!otaRunningStateAllowsUpdate(
           OTA_STATE_QUERY_OK, bootstrapState, false),
         "undefined state with configured/running layout mismatch must fail");
  const uint32_t rejectedStates[] = {
    OTA_IMAGE_STATE_NEW_RAW,
    OTA_IMAGE_STATE_PENDING_VERIFY_RAW,
    OTA_IMAGE_STATE_INVALID_RAW,
    OTA_IMAGE_STATE_ABORTED_RAW,
    0x12345678U,
  };
  for (uint32_t rawState : rejectedStates) {
    expect(!otaRunningStateAllowsUpdate(
             OTA_STATE_QUERY_OK,
             otaRunningImageStateFromRaw(rawState),
             true),
           "NEW/PENDING/INVALID/ABORTED/unknown states must stay fail-closed");
  }

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

  const char validHeader[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n"
    "Content-Encoding: identity\r\nConnection: close\r\n\r\n";
  OtaHttpMeta parsed = {};
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(validHeader),
           sizeof(validHeader) - 1,
           &parsed) && otaHttpMetaExactValid(parsed, 1000, 1000),
         "bounded manual parser should accept strict identity response");
  const char chunkedHeader[] =
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(chunkedHeader),
           sizeof(chunkedHeader) - 1,
           &parsed) && !parsed.identityEncoding,
         "manual parser must expose forbidden transfer encoding");
  const char duplicateLength[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n";
  expect(!otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(duplicateLength),
           sizeof(duplicateLength) - 1,
           &parsed),
         "duplicate Content-Length must fail closed");
  const char mixedCase[] =
    "HTTP/1.1 200 OK\r\ncOnTeNt-LeNgTh:\t7 \r\n\r\n";
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(mixedCase),
           sizeof(mixedCase) - 1,
           &parsed) && parsed.contentLength == 7,
         "header names must be case-insensitive with bounded OWS");
  const char compressed[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 7\r\n"
    "Content-Encoding: gzip\r\n\r\n";
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(compressed),
           sizeof(compressed) - 1,
           &parsed) && !parsed.identityEncoding,
         "compressed bodies must be exposed for rejection");
  const char redirect[] =
    "HTTP/1.1 302 Found\r\nContent-Length: 7\r\n"
    "Location: https://192.168.1.2/elsewhere\r\n\r\n";
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(redirect),
           sizeof(redirect) - 1,
           &parsed) && !otaHttpMetaBoundedValid(parsed, 7),
         "redirect status and Location must fail strict response validation");
  const char informational[] =
    "HTTP/1.1 100 Continue\r\nContent-Length: 7\r\n\r\n";
  expect(otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(informational),
           sizeof(informational) - 1,
           &parsed) && !otaHttpMetaBoundedValid(parsed, 7),
         "informational responses must not be accepted as OTA bodies");
  const char bareLf[] =
    "HTTP/1.1 200 OK\nContent-Length: 7\r\n\r\n";
  expect(!otaParseStrictHttpResponseHeader(
           reinterpret_cast<const uint8_t*>(bareLf),
           sizeof(bareLf) - 1,
           &parsed),
         "manual parser must require CRLF on every response line");
  uint8_t oversizedHeader[OTA_UPDATE_HTTP_HEADER_MAX_BYTES + 1] = {};
  memcpy(
    oversizedHeader + sizeof(oversizedHeader) - 4,
    "\r\n\r\n",
    4
  );
  expect(!otaParseStrictHttpResponseHeader(
           oversizedHeader, sizeof(oversizedHeader), &parsed),
         "header parser must reject bytes beyond its fixed cap");
  expect(otaPrefetchedBodyValid(107, 100, 7),
         "prefetched bytes exactly matching Content-Length may proceed");
  expect(!otaPrefetchedBodyValid(108, 100, 7),
         "known extra body bytes must fail during OPEN before Flash begin");
  expect(otaHttpEofDecision(-0x6900, -0x6900, -0x6880) ==
           OTA_HTTP_EOF_WAIT,
         "WANT_READ while awaiting close must remain non-blocking");
  expect(otaHttpEofDecision(-0x6880, -0x6900, -0x6880) ==
           OTA_HTTP_EOF_WAIT,
         "WANT_WRITE while awaiting close must remain non-blocking");
  expect(otaHttpEofDecision(0, -0x6900, -0x6880) ==
           OTA_HTTP_EOF_COMPLETE,
         "only authenticated TLS EOF completes the exact body");
  expect(otaHttpEofDecision(1, -0x6900, -0x6880) ==
           OTA_HTTP_EOF_FAILURE,
         "a later extra byte must fail before staging completes");
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
  bool restartReturns;
  uint8_t pendingOpenCount;
  bool failOpenAfterPending;
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
  if ((event == OTA_EVENT_RESTART || event == OTA_EVENT_RESTART_FALLBACK) &&
      fake->restartReturns)
    return otaActionFailure();
  if (event == OTA_EVENT_OPEN_MANIFEST ||
      event == OTA_EVENT_OPEN_SIGNATURE ||
      event == OTA_EVENT_OPEN_FIRMWARE) {
    if (fake->pendingOpenCount) {
      --fake->pendingOpenCount;
      return otaActionPending();
    }
    if (fake->failOpenAfterPending) return otaActionFailure();
  }
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
  for (int i = 0; i < 100 && !otaUpdateTerminal(*machine); ++i) {
    otaUpdateStep(machine, inputs, confirm && i == 0, false);
    inputs.nowMs += OTA_UPDATE_RESTART_RETRY_MS;
    if (machine->bootCommitted) break;
  }
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
  };
  expect(fake.eventCount == sizeof(expected) / sizeof(expected[0]),
         "successful OTA should have the exact bounded event count");
  expect(memcmp(fake.events, expected, sizeof(expected)) == 0,
         "successful OTA event order must be strict");
  expect(update.bootCommitted && update.phase == OTA_PHASE_RESTART,
         "successful set_boot must latch an irreversible restart-only state");
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
  expect(update.bootCommitted && update.phase == OTA_PHASE_RESTART,
         "pending and partial reads should reach restart-only state");
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

static void testAsyncOpenRemainsResponsiveAndPreBegin() {
  Fake pending = {};
  pending.digestMatches = true;
  pending.pendingOpenCount = 20;
  OtaUpdateMachine first = machine(&pending);
  OtaUpdateInputs inputs = ready();
  otaUpdateStep(&first, inputs, true, false);
  for (int poll = 0; poll < 20; ++poll) {
    otaUpdateStep(&first, inputs, false, false);
    expect(first.phase == OTA_PHASE_OPEN_MANIFEST &&
             !saw(pending, OTA_EVENT_BEGIN),
           "async TLS open may remain pending across polls without Flash begin");
  }
  size_t beforeCancel = pending.eventCount;
  inputs.prompt = true;
  otaUpdateStep(&first, inputs, false, true);
  expect(first.phase == OTA_PHASE_CANCELLED &&
           pending.eventCount == beforeCancel,
         "physical cancel or approval must stop a pending open immediately");
  inputs.prompt = false;
  otaUpdateStep(&first, inputs, false, false);
  expect(first.phase == OTA_PHASE_CANCELLED &&
           pending.eventCount == beforeCancel,
         "late async completion after cancel must be ignored");

  Fake conflict = {};
  conflict.digestMatches = true;
  conflict.pendingOpenCount = 10;
  OtaUpdateMachine conflicted = machine(&conflict);
  inputs = ready();
  otaUpdateStep(&conflicted, inputs, true, false);
  otaUpdateStep(&conflicted, inputs, false, false);
  size_t beforeConflict = conflict.eventCount;
  inputs.prompt = true;
  otaUpdateStep(&conflicted, inputs, false, false);
  expect(conflicted.phase == OTA_PHASE_CANCELLED &&
           conflict.eventCount == beforeConflict,
         "approval conflict must preempt a pending TLS open without polling it");

  Fake timeout = {};
  timeout.digestMatches = true;
  timeout.pendingOpenCount = 3;
  timeout.failOpenAfterPending = true;
  OtaUpdateMachine second = machine(&timeout);
  otaUpdateStep(&second, ready(), true, false);
  for (int poll = 0; poll < 4; ++poll)
    otaUpdateStep(&second, ready(), false, false);
  expect(second.phase == OTA_PHASE_ERROR &&
           !saw(timeout, OTA_EVENT_BEGIN),
         "hard open timeout/failure must terminate without Flash begin");

  Fake firmware = {};
  firmware.digestMatches = true;
  OtaUpdateMachine third = machine(&firmware);
  otaUpdateStep(&third, ready(), true, false);
  while (third.phase != OTA_PHASE_OPEN_FIRMWARE)
    otaUpdateStep(&third, ready(), false, false);
  firmware.pendingOpenCount = 5;
  for (int poll = 0; poll < 5; ++poll) {
    otaUpdateStep(&third, ready(), false, false);
    expect(third.phase == OTA_PHASE_OPEN_FIRMWARE &&
             !saw(firmware, OTA_EVENT_BEGIN),
           "firmware TLS/header open must complete before esp_ota_begin");
  }
}

static void testBootCommitIgnoresAllCancellationAndGateChanges() {
  Fake fake = {};
  fake.digestMatches = true;
  fake.restartReturns = true;
  OtaUpdateMachine update = machine(&fake);
  OtaUpdateInputs inputs = ready();
  tickUntilTerminal(&update, inputs);
  expect(update.bootCommitted && count(fake, OTA_EVENT_SET_BOOT) == 1,
         "test must first commit the verified boot partition exactly once");
  size_t committedEventCount = fake.eventCount;

  auto expectRestartOnly = [&](OtaUpdateInputs changed, bool cancel,
                               const char* message) {
    size_t before = fake.eventCount;
    changed.nowMs = update.nextRestartAttemptMs;
    otaUpdateStep(&update, changed, false, cancel);
    OtaUpdateEvent last = fake.events[fake.eventCount - 1];
    expect(update.bootCommitted && update.phase == OTA_PHASE_RESTART &&
             !otaUpdateTerminal(update) && fake.eventCount == before + 1 &&
             (last == OTA_EVENT_RESTART ||
              last == OTA_EVENT_RESTART_FALLBACK),
           message);
  };
  expectRestartOnly(inputs, true, "post-commit physical B must be ignored");
  OtaUpdateInputs changed = inputs;
  changed.prompt = true;
  expectRestartOnly(changed, false, "post-commit approval prompt must be ignored");
  changed = inputs;
  changed.transfer = true;
  expectRestartOnly(changed, false, "post-commit transfer conflict must be ignored");
  changed = inputs;
  changed.provisioning = true;
  expectRestartOnly(changed, false, "post-commit provisioning conflict must be ignored");
  changed = inputs;
  changed.passkey = true;
  expectRestartOnly(changed, false, "post-commit passkey conflict must be ignored");
  changed = inputs;
  changed.functional = true;
  expectRestartOnly(changed, false, "post-commit functional conflict must be ignored");
  changed = inputs;
  changed.wifiProvisioned = false;
  expectRestartOnly(changed, false, "post-commit provisioning gate drop must be ignored");
  changed = inputs;
  changed.wifiOnline = false;
  expectRestartOnly(changed, false, "post-commit Wi-Fi drop must be ignored");
  changed = inputs;
  changed.trustedTime = false;
  expectRestartOnly(changed, false, "post-commit trusted-time drop must be ignored");
  changed = inputs;
  changed.externalPower = false;
  changed.batteryKnown = false;
  expectRestartOnly(changed, false, "post-commit power drop must be ignored");
  changed = inputs;
  changed.offerPending = false;
  changed.offerFresh = false;
  changed.bleConnected = false;
  expectRestartOnly(changed, false, "post-commit coordination drop must be ignored");
  expect(fake.eventCount > committedEventCount,
         "restart-only checks must exercise the committed state");
  expect(count(fake, OTA_EVENT_ABORT) == 0,
         "post-commit state must never abort or clean staged flash");
  expect(saw(fake, OTA_EVENT_RESTART_FALLBACK),
         "bounded normal restart returns must escalate to the fallback reset");
  expect(update.bootCommitted && !otaUpdateTerminal(update) &&
           update.phase != OTA_PHASE_ERROR && update.phase != OTA_PHASE_CANCELLED,
         "even a returning fallback reset remains irreversible restart-only state");
}

static void testTerminalScrubPreservesOnlyOutcome() {
  Fake fake = {};
  OtaUpdateMachine update = machine(&fake, 4096);
  update.phase = OTA_PHASE_ERROR;
  update.receivedBytes = 123;
  update.readbackBytes = 45;
  update.pendingChunkBytes = 6;
  update.handleValid = true;
  otaUpdateScrubMachine(&update);
  expect(update.phase == OTA_PHASE_ERROR && !update.bootCommitted,
         "pre-commit terminal scrub must preserve only the outcome");
  expect(update.action == nullptr && update.actionContext == nullptr &&
           update.imageSize == 0 && update.receivedBytes == 0 &&
           update.readbackBytes == 0 && update.pendingChunkBytes == 0 &&
           !update.handleValid,
         "terminal scrub must remove lengths, handles, and callback pointers");
}

static void testDisplayMetadataPersistsUntilUiLeaves() {
  OtaDisplayMetadata display = otaDisplayMetadataInitial();
  expect(!display.visible && display.version[0] == 0 && display.sizeBytes == 0,
         "display metadata begins empty");
  expect(otaDisplayMetadataCapture(&display, "0.1.5", 123456),
         "confirmed offer metadata is captured");
  expect(display.visible && strcmp(display.version, "0.1.5") == 0 &&
           display.sizeBytes == 123456,
         "offer metadata remains visible before authentication");
  expect(otaDisplayMetadataCapture(&display, "0.1.5", 123456),
         "authenticated manifest refreshes the same display metadata");
  expect(strcmp(display.version, "0.1.5") == 0 && display.sizeBytes == 123456,
         "manifest metadata survives authentication");
  otaDisplayMetadataPreserveForBootCommit(&display);
  expect(display.visible && strcmp(display.version, "0.1.5") == 0 &&
           display.sizeBytes == 123456,
         "boot commit does not scrub non-sensitive display metadata");
  otaDisplayMetadataClear(&display);
  expect(!display.visible && display.version[0] == 0 && display.sizeBytes == 0,
         "metadata clears only when the OTA UI leaves");
}

static void testCancellationApplicabilityAndFailureMapping() {
  OtaUpdateMachine update = otaUpdateMachineInitial();
  expect(otaUpdateCancellationAllowed(true, update),
         "confirmation is cancellable");
  update.phase = OTA_PHASE_READBACK;
  expect(otaUpdateCancellationAllowed(true, update),
         "readback remains cancellable before final boot selection");
  update.phase = OTA_PHASE_SET_BOOT;
  expect(!otaUpdateCancellationAllowed(true, update),
         "set-boot is not cancellable even before its action runs");
  update.phase = OTA_PHASE_RESTART;
  expect(!otaUpdateCancellationAllowed(true, update),
         "restart is irreversible");
  update.phase = OTA_PHASE_RESTARTING;
  expect(!otaUpdateCancellationAllowed(true, update),
         "restarting is irreversible");
  update.phase = OTA_PHASE_CANCELLED;
  expect(!otaUpdateCancellationAllowed(true, update),
         "cancelled terminal state cannot be cancelled again");
  update.phase = OTA_PHASE_ERROR;
  expect(!otaUpdateCancellationAllowed(true, update),
         "error terminal state cannot be relabelled cancelled");
  update = otaUpdateMachineInitial();
  expect(!otaUpdateCancellationAllowed(false, update),
         "inactive update cannot accept cancellation");

  expect(otaUpdateFailureForGate(OTA_GATE_WIFI, false) == OTA_UPDATE_FAILURE_WIFI,
         "Wi-Fi gate maps to wifi");
  expect(otaUpdateFailureForGate(OTA_GATE_POWER, false) == OTA_UPDATE_FAILURE_POWER,
         "power gate maps to power");
  expect(otaUpdateFailureForGate(OTA_GATE_CONFLICT, false) == OTA_UPDATE_FAILURE_CONFLICT,
         "coordination conflict maps to conflict");
  expect(otaUpdateFailureForGate(OTA_GATE_WIFI, true) == OTA_UPDATE_FAILURE_TIMEOUT,
         "expired readiness deadline maps to timeout");
  expect(otaUpdateFailureForEvent(OTA_EVENT_OPEN_MANIFEST) == OTA_UPDATE_FAILURE_TRUST,
         "TLS open failures map to trust");
  expect(otaUpdateFailureForEvent(OTA_EVENT_READ_MANIFEST) == OTA_UPDATE_FAILURE_MANIFEST,
         "manifest HTTP body failures map to manifest");
  expect(otaUpdateFailureForEvent(OTA_EVENT_DOWNLOAD) == OTA_UPDATE_FAILURE_DOWNLOAD,
         "firmware reads map to download");
  expect(otaUpdateFailureForEvent(OTA_EVENT_DIGEST_MATCH) == OTA_UPDATE_FAILURE_HASH,
         "digest mismatch maps to hash");
  expect(otaUpdateFailureForEvent(OTA_EVENT_SET_BOOT) == OTA_UPDATE_FAILURE_ROLLBACK,
         "boot selection maps to rollback");
  expect(otaUpdateFailureForManifest(OTA_MANIFEST_SIGNATURE_INVALID) == OTA_UPDATE_FAILURE_TRUST,
         "signature rejection maps to trust");
  expect(otaUpdateFailureForManifest(OTA_MANIFEST_VERSION_INVALID) == OTA_UPDATE_FAILURE_VERSION,
         "invalid version maps to version");
  expect(otaUpdateFailureForManifest(OTA_MANIFEST_NOT_NEWER) == OTA_UPDATE_FAILURE_VERSION,
         "non-newer version maps to version");
  expect(otaUpdateFailureForManifest(OTA_MANIFEST_NON_CANONICAL) == OTA_UPDATE_FAILURE_MANIFEST,
         "manifest schema failures map to manifest");
}

int main() {
  testSignedOfferPolicy();
  testDirectOfferStartsAtomically();
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
  testAsyncOpenRemainsResponsiveAndPreBegin();
  testBootCommitIgnoresAllCancellationAndGateChanges();
  testTerminalScrubPreservesOnlyOutcome();
  testDisplayMetadataPersistsUntilUiLeaves();
  testCancellationApplicabilityAndFailureMapping();
  puts("ota update logic tests passed");
  return 0;
}
