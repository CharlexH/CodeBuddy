#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ota_authorization_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static const char* MANIFEST_URL =
  "https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.json";
static const char* SIGNATURE_URL =
  "https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.sig";
static const char* DER_HEX = "3006020101020101";

static OtaAuthorizationInput validAuthorization() {
  OtaAuthorizationInput input = {};
  input.action = "code-buddy-firmware-install-v1";
  input.device = "Codex-4DAD";
  input.expiresAt = 1720000120;
  input.generation = 9;
  input.issuedAt = 1720000000;
  input.manifestUrl = MANIFEST_URL;
  input.nonce = "abcdefghijklmnopqrstuvwx";
  input.signatureUrl = SIGNATURE_URL;
  input.sizeBytes = 1234;
  input.version = "1.2.3";
  input.authorizationHex = DER_HEX;
  return input;
}

struct VerifyContext {
  bool allow;
  bool called;
  uint8_t expectedSignature[8];
};

static bool verifyCallback(
  const uint8_t* canonical,
  size_t canonicalLength,
  const uint8_t* signature,
  size_t signatureLength,
  void* opaque
) {
  VerifyContext* context = static_cast<VerifyContext*>(opaque);
  context->called = true;
  return context->allow && canonical && canonicalLength > 0 &&
    signatureLength == sizeof(context->expectedSignature) &&
    memcmp(signature, context->expectedSignature, signatureLength) == 0;
}

static VerifyContext verifier(bool allow = true) {
  VerifyContext context = {allow, false, {0x30, 0x06, 0x02, 0x01, 0x01, 0x02, 0x01, 0x01}};
  return context;
}

static void testCanonicalBytesMatchHostContract() {
  OtaAuthorizationInput input = validAuthorization();
  char canonical[OTA_AUTHORIZATION_MAX_BYTES];
  size_t length = otaAuthorizationCanonicalBytes(input, canonical, sizeof(canonical));
  const char* expected =
    "{\"action\":\"code-buddy-firmware-install-v1\",\"device\":\"Codex-4DAD\","
    "\"expiresAt\":1720000120,\"generation\":9,\"issuedAt\":1720000000,"
    "\"manifestUrl\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.json\","
    "\"nonce\":\"abcdefghijklmnopqrstuvwx\","
    "\"signatureUrl\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.sig\","
    "\"sizeBytes\":1234,\"version\":\"1.2.3\"}";
  expect_true(length == strlen(expected) && strcmp(canonical, expected) == 0,
              "canonical authorization must exactly match host sort-key ASCII JSON");
  char truncated[32];
  memset(truncated, 'x', sizeof(truncated));
  expect_true(otaAuthorizationCanonicalBytes(input, truncated, sizeof(truncated)) == 0 &&
              truncated[0] == 0,
              "canonical reconstruction must fail closed on a short output buffer");
}

static void testDerHexBoundaries() {
  uint8_t decoded[OTA_SIGNATURE_MAX_BYTES] = {};
  size_t decodedLength = 0;
  expect_true(otaAuthorizationDecodeDerHex(
                DER_HEX, decoded, sizeof(decoded), &decodedLength) &&
              decodedLength == 8 && decoded[0] == 0x30 && decoded[7] == 0x01,
              "valid lowercase DER hex should decode exactly");
  const char* invalid[] = {
    "", "300602010102010", "300602010102010g", "300602010102010A",
    "3007020101020101", "3106020101020101", "3000"
  };
  for (const char* value : invalid) {
    decodedLength = 99;
    expect_true(!otaAuthorizationDecodeDerHex(
                  value, decoded, sizeof(decoded), &decodedLength) && decodedLength == 0,
                "malformed, odd, uppercase, or structurally invalid DER hex must fail");
  }
  char oversized[OTA_SIGNATURE_MAX_BYTES * 2 + 3];
  oversized[0] = '3'; oversized[1] = '0'; oversized[2] = '9'; oversized[3] = 'e';
  memset(oversized + 4, '0', sizeof(oversized) - 5);
  oversized[sizeof(oversized) - 1] = 0;
  expect_true(!otaAuthorizationDecodeDerHex(
                oversized, decoded, sizeof(decoded), &decodedLength),
              "DER hex beyond the bounded signature buffer must fail");
  expect_true(!otaAuthorizationDecodeDerHex(
                DER_HEX, decoded, 7, &decodedLength),
              "DER decode must respect caller capacity");
}

static void testSignatureDeviceAndTimeChecks() {
  OtaAuthorizationInput input = validAuthorization();
  OtaAuthorizationReplayState replay = otaAuthorizationReplayInitial();
  VerifyContext context = verifier();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_OK &&
              context.called,
              "matching device, trusted current time, and good signature should pass");

  replay = otaAuthorizationReplayInitial();
  context = verifier(false);
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_SIGNATURE_INVALID &&
              context.called && !replay.entries[0].valid,
              "bad signature must fail without polluting replay state");

  replay = otaAuthorizationReplayInitial();
  context = verifier();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-BEEF", true, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_DEVICE_MISMATCH &&
              !context.called,
              "offer must bind to the device name derived from its BT MAC");
  input.device = "codex-4dad";
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "codex-4dad", true, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_FIELD_INVALID,
              "device format must be exact Codex-XXXX uppercase hexadecimal");

  input = validAuthorization();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", false, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_TIME_UNTRUSTED,
              "signed authorization must fail closed without trusted epoch");
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, input.issuedAt - 1,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_NOT_YET_VALID,
              "future authorization must fail");
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, input.expiresAt,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_EXPIRED,
              "authorization is expired at expiresAt");
  input.expiresAt = input.issuedAt;
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, input.issuedAt,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_FIELD_INVALID,
              "empty or reversed authorization windows must fail");
  input = validAuthorization();
  input.expiresAt = input.issuedAt + OTA_AUTHORIZATION_MAX_LIFETIME_SECONDS + 1;
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, input.issuedAt,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_FIELD_INVALID,
              "authorization lifetime above the bounded maximum must fail");
  input.expiresAt = input.issuedAt + 1;
  context = verifier();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, input.issuedAt,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_OK,
              "one-second authorization lifetime should pass at its issued boundary");
}

static void testReplayAndFieldValidation() {
  expect_true(otaAuthorizationSignedOfferShapeValid(10) &&
              !otaAuthorizationSignedOfferShapeValid(9) &&
              !otaAuthorizationSignedOfferShapeValid(11),
              "signed offer must have exactly ten fields; missing or excess fields fail");
  OtaAuthorizationInput input = validAuthorization();
  OtaAuthorizationReplayState replay = otaAuthorizationReplayInitial();
  VerifyContext context = verifier();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, 1720000060,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_OK,
              "first authorization should pass");
  context = verifier();
  expect_true(otaAuthorizationVerifyAndRemember(
                input, "Codex-4DAD", true, 1720000061,
                verifyCallback, &context, &replay) == OTA_AUTHORIZATION_REPLAY &&
              !context.called,
              "same nonce and generation must be rejected before crypto work");

  OtaAuthorizationInput malformed[] = {
    validAuthorization(), validAuthorization(), validAuthorization(),
    validAuthorization(), validAuthorization(), validAuthorization(),
    validAuthorization(), validAuthorization(), validAuthorization(),
  };
  malformed[0].action = "firmware-install";
  malformed[1].nonce = "short";
  malformed[2].generation = 0;
  malformed[3].version = "01.2.3";
  malformed[4].sizeBytes = 0;
  malformed[5].manifestUrl = SIGNATURE_URL;
  malformed[6].signatureUrl = MANIFEST_URL;
  malformed[7].authorizationHex = nullptr;
  malformed[8].device = nullptr;
  for (const OtaAuthorizationInput& invalid : malformed) {
    OtaAuthorizationReplayState clean = otaAuthorizationReplayInitial();
    VerifyContext verify = verifier();
    expect_true(otaAuthorizationVerifyAndRemember(
                  invalid, "Codex-4DAD", true, 1720000060,
                  verifyCallback, &verify, &clean) != OTA_AUTHORIZATION_OK,
                "malformed or missing signed authorization field must fail");
  }
}

static void testVerifiedOfferCreatesSeparateBoundedWindow() {
  OtaOfferState offer = otaOfferStateInitial();
  expect_true(!otaOfferAcceptBoundHint(
                "abcdefghijklmnopqrstuvwx", 9, "1.2.3", 1234,
                MANIFEST_URL, SIGNATURE_URL, "1.2.2", 1000,
                true, false, false, false, true, 100, &offer
              ) && !offer.windowOpen,
              "legacy unsigned offer must still require a physical receive window");
  expect_true(otaOfferAcceptVerifiedBoundHint(
                "abcdefghijklmnopqrstuvwx", 9, "1.2.3", 1234,
                MANIFEST_URL, SIGNATURE_URL, "1.2.2", 1000,
                true, false, false, false, true, 100, &offer
              ) && offer.pending && offer.windowOpen && offer.signedAuthorized,
              "already verified signed offer may create its own bounded receive state");
  otaOfferReject(&offer);
  expect_true(!offer.pending && !offer.windowOpen && !offer.signedAuthorized,
              "reject must scrub signed authorization state");

  OtaAuthorizationInput input = validAuthorization();
  OtaAuthorizationReplayState replay = otaAuthorizationReplayInitial();
  VerifyContext context = verifier(false);
  OtaAuthorizationResult result = OTA_AUTHORIZATION_OK;
  expect_true(!otaAuthorizationVerifyThenAccept(
                input, "Codex-4DAD", true, 1720000060,
                verifyCallback, &context, &replay, "1.2.2", 1000,
                true, false, false, false, true, 100, &offer, &result
              ) && result == OTA_AUTHORIZATION_SIGNATURE_INVALID &&
              !offer.pending && !offer.windowOpen && !offer.signedAuthorized,
              "invalid signed offer must not create UI/network-capable receive state");

  context = verifier();
  expect_true(otaAuthorizationVerifyThenAccept(
                input, "Codex-4DAD", true, 1720000060,
                verifyCallback, &context, &replay, "1.2.2", 1000,
                true, false, false, false, true, 100, &offer, &result
              ) && result == OTA_AUTHORIZATION_OK && offer.pending &&
              offer.signedAuthorized,
              "valid signed offer should establish bounded state only after verification");
}

int main() {
  testCanonicalBytesMatchHostContract();
  testDerHexBoundaries();
  testSignatureDeviceAndTimeChecks();
  testReplayAndFieldValidation();
  testVerifiedOfferCreatesSeparateBoundedWindow();
  puts("ota_authorization_logic_test: ok");
  return 0;
}
