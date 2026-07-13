#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "firmware_version.h"
#include "ota_manifest_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

static const char* VALID_URL =
  "https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin";
static const char* VALID_MANIFEST_URL =
  "https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.json";
static const char* VALID_SIGNATURE_URL =
  "https://192.168.44.8:49321/0123456789abcdefghijklmn/manifest.sig";

static void canonicalManifest(
  char* out,
  size_t outSize,
  const char* version = "1.2.3",
  const char* size = "1234",
  const char* digest =
    "abababababababababababababababababababababababababababababababab",
  const char* url = nullptr
) {
  snprintf(
    out,
    outSize,
    "{\"artifact\":{\"sha256\":\"%s\",\"sizeBytes\":%s,\"url\":\"%s\"},"
    "\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"%s\"}",
    digest,
    size,
    url ? url : VALID_URL,
    version
  );
}

static OtaManifestResult parse(
  const char* raw,
  OtaManifestDescriptor* descriptor,
  const char* current = "1.2.2"
) {
  return otaManifestParseCanonical(
    reinterpret_cast<const uint8_t*>(raw),
    strlen(raw),
    current,
    descriptor
  );
}

static void testExactCanonicalManifestAndDetachedDescriptor() {
  char raw[OTA_MANIFEST_MAX_BYTES + 1];
  canonicalManifest(raw, sizeof(raw));
  OtaManifestDescriptor descriptor = {};
  expect_true(parse(raw, &descriptor) == OTA_MANIFEST_OK,
              "exact host canonical manifest should parse");
  expect_true(strcmp(descriptor.version, "1.2.3") == 0,
              "version should be copied into the descriptor");
  expect_true(descriptor.sizeBytes == 1234,
              "size should be copied into the descriptor");
  expect_true(strcmp(descriptor.sha256,
    "abababababababababababababababababababababababababababababababab") == 0,
    "digest should be copied into the descriptor");
  expect_true(strcmp(descriptor.artifactUrl, VALID_URL) == 0,
              "artifact URL should be copied into the descriptor");

  memset(raw, 'x', strlen(raw));
  expect_true(strcmp(descriptor.version, "1.2.3") == 0 &&
              strcmp(descriptor.artifactUrl, VALID_URL) == 0,
              "descriptor must not retain pointers into mutable manifest bytes");
}

static void testAuthenticationAlwaysPrecedesParsing() {
  char raw[OTA_MANIFEST_MAX_BYTES + 1];
  canonicalManifest(raw, sizeof(raw));
  const uint8_t signature[] = {0x30, 0x01, 0x00};
  OtaManifestDescriptor descriptor = {};
  struct Context { bool called; bool allow; } context = {false, false};
  auto verifier = [](
    const uint8_t* manifest,
    size_t manifestLength,
    const uint8_t* signatureBytes,
    size_t signatureLength,
    void* opaque
  ) -> bool {
    Context* state = static_cast<Context*>(opaque);
    state->called = true;
    return state->allow && manifest && manifestLength > 0 && signatureBytes &&
      signatureLength == 3;
  };

  expect_true(
    otaManifestAuthenticateWith(
      reinterpret_cast<const uint8_t*>(raw), strlen(raw),
      signature, sizeof(signature), "1.2.2", verifier, &context, &descriptor
    ) == OTA_MANIFEST_SIGNATURE_INVALID,
    "a failed detached signature must prevent parsing"
  );
  expect_true(context.called && descriptor.version[0] == 0,
              "failed authentication must not expose parsed fields");

  context.allow = true;
  expect_true(
    otaManifestAuthenticateWith(
      reinterpret_cast<const uint8_t*>(raw), strlen(raw),
      signature, sizeof(signature), "1.2.2", verifier, &context, &descriptor
    ) == OTA_MANIFEST_OK,
    "valid detached signature should allow strict parsing"
  );
}

static void testCanonicalBytesRejectAmbiguity() {
  char valid[OTA_MANIFEST_MAX_BYTES + 1];
  canonicalManifest(valid, sizeof(valid));
  const char* invalid[] = {
    "{\"artifact\":{\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":1234,\"url\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"1.2.3\",\"extra\":1}",
    "{\"artifact\":{\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":1234,\"url\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"1.2.3\"}",
    "{\"schema\":1,\"version\":\"1.2.3\",\"chip\":\"esp32s3\",\"artifact\":{\"url\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin\",\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":1234}}",
    "{ \"artifact\":{\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":1234,\"url\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"1.2.3\"}",
    "{\"artifact\":{\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":\"1234\",\"url\":\"https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"1.2.3\"}",
    "{\"artifact\":{\"sha256\":\"abababababababababababababababababababababababababababababababab\",\"sizeBytes\":1234,\"url\":\"https:\\/\\/192.168.44.8:49321\\/0123456789abcdefghijklmn\\/firmware.bin\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\"1.2.3\"}",
  };
  OtaManifestDescriptor descriptor = {};
  for (const char* raw : invalid) {
    expect_true(parse(raw, &descriptor) != OTA_MANIFEST_OK,
                "non-canonical, duplicate, retyped, reordered, or unknown fields must fail");
  }

  uint8_t withBom[OTA_MANIFEST_MAX_BYTES] = {0xef, 0xbb, 0xbf};
  size_t validLength = strlen(valid);
  memcpy(withBom + 3, valid, validLength);
  expect_true(otaManifestParseCanonical(withBom, validLength + 3, "1.2.2", &descriptor)
                != OTA_MANIFEST_OK,
              "UTF-8 BOM must fail");
  uint8_t withNul[OTA_MANIFEST_MAX_BYTES];
  memcpy(withNul, valid, validLength);
  withNul[validLength / 2] = 0;
  expect_true(otaManifestParseCanonical(withNul, validLength, "1.2.2", &descriptor)
                != OTA_MANIFEST_OK,
              "embedded NUL must fail");
  uint8_t invalidUtf8[OTA_MANIFEST_MAX_BYTES];
  memcpy(invalidUtf8, valid, validLength);
  invalidUtf8[validLength / 2] = 0xff;
  expect_true(otaManifestParseCanonical(invalidUtf8, validLength, "1.2.2", &descriptor)
                != OTA_MANIFEST_OK,
              "invalid UTF-8 must fail");
  expect_true(otaManifestParseCanonical(
                reinterpret_cast<const uint8_t*>(valid), OTA_MANIFEST_MAX_BYTES + 1,
                "1.2.2", &descriptor) == OTA_MANIFEST_TOO_LARGE,
              "oversized raw manifest must fail before access");
}

static void testTypedFieldsAndMonotonicVersion() {
  char raw[OTA_MANIFEST_MAX_BYTES + 1];
  OtaManifestDescriptor descriptor = {};

  const char* invalidVersions[] = {
    "1.2", "01.2.3", "1.02.3", "1.2.03", "v1.2.3", "1.2.3-01",
    "1.2.3-", "1.2.3+", "1.2.3..4", "4294967296.0.0",
  };
  for (const char* version : invalidVersions) {
    canonicalManifest(raw, sizeof(raw), version);
    expect_true(parse(raw, &descriptor) != OTA_MANIFEST_OK,
                "firmware semantic version subset must match host validation");
  }
  canonicalManifest(raw, sizeof(raw), "1.2.2");
  expect_true(parse(raw, &descriptor, "1.2.2") == OTA_MANIFEST_NOT_NEWER,
              "equal version must fail");
  canonicalManifest(raw, sizeof(raw), "1.2.1");
  expect_true(parse(raw, &descriptor, "1.2.2") == OTA_MANIFEST_NOT_NEWER,
              "downgrade must fail");
  canonicalManifest(raw, sizeof(raw), "1.2.3+build.7");
  expect_true(parse(raw, &descriptor, "1.2.3") == OTA_MANIFEST_NOT_NEWER,
              "build metadata alone must not create a newer release");
  canonicalManifest(raw, sizeof(raw), "1.2.3");
  expect_true(parse(raw, &descriptor, "1.2.3-rc.9") == OTA_MANIFEST_OK,
              "stable release must be newer than its prerelease");

  const char* invalidSizes[] = {"0", "01", "4294967296", "3342337"};
  for (const char* size : invalidSizes) {
    canonicalManifest(raw, sizeof(raw), "1.2.3", size);
    expect_true(parse(raw, &descriptor) != OTA_MANIFEST_OK,
                "zero, non-canonical, overflowing, or slot-oversized image must fail");
  }
  canonicalManifest(raw, sizeof(raw), "1.2.3", "3342336");
  expect_true(parse(raw, &descriptor) == OTA_MANIFEST_OK,
              "exact OTA slot capacity should be accepted");

  canonicalManifest(raw, sizeof(raw), "1.2.3", "1234",
                    "ABABABABABABABABABABABABABABABABABABABABABABABABABABABABABAB");
  expect_true(parse(raw, &descriptor) != OTA_MANIFEST_OK,
              "digest must be exactly lowercase hexadecimal");

  char maxVersion[OTA_VERSION_MAX_BYTES];
  memcpy(maxVersion, "1.2.3-", 6);
  memset(maxVersion + 6, 'a', 57);
  maxVersion[63] = 0;
  canonicalManifest(raw, sizeof(raw), maxVersion);
  expect_true(parse(raw, &descriptor) == OTA_MANIFEST_OK,
              "63-byte semantic version boundary should pass");
  char oversizedVersion[OTA_VERSION_MAX_BYTES + 1];
  memcpy(oversizedVersion, maxVersion, 63);
  oversizedVersion[63] = 'a';
  oversizedVersion[64] = 0;
  canonicalManifest(raw, sizeof(raw), oversizedVersion);
  expect_true(parse(raw, &descriptor) != OTA_MANIFEST_OK,
              "64-byte semantic version must fail");
}

static void testMacLocalUrlPolicyAndBinding() {
  expect_true(OTA_MANIFEST_MAX_BYTES == 1024,
              "manifest byte cap must match the bounded line transport");
  expect_true(otaLocalHttpsUrlValid(VALID_URL, OTA_URL_FIRMWARE),
              "RFC1918 HTTPS firmware URL with explicit port/token should pass");
  expect_true(otaLocalHttpsUrlValid(VALID_MANIFEST_URL, OTA_URL_MANIFEST),
              "matching manifest URL should pass");
  expect_true(otaLocalHttpsUrlValid(VALID_SIGNATURE_URL, OTA_URL_SIGNATURE),
              "matching signature URL should pass");

  const char* invalid[] = {
    "http://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin",
    "https://8.8.8.8:49321/0123456789abcdefghijklmn/firmware.bin",
    "https://127.0.0.1:49321/0123456789abcdefghijklmn/firmware.bin",
    "https://169.254.1.1:49321/0123456789abcdefghijklmn/firmware.bin",
    "https://192.168.44.8/0123456789abcdefghijklmn/firmware.bin",
    "https://192.168.44.8:0/0123456789abcdefghijklmn/firmware.bin",
    "https://192.168.44.8:049321/0123456789abcdefghijklmn/firmware.bin",
    "https://192.168.44.8:65536/0123456789abcdefghijklmn/firmware.bin",
    "https://user@192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin",
    "https://192.168.44.8:49321/short-token/firmware.bin",
    "https://192.168.44.8:49321/0123456789abcdefghijklmn/../firmware.bin",
    "https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin?x=1",
    "https://192.168.44.8:49321/0123456789abcdefghijklmn/firmware.bin#x",
    "https://192.168.44.8:49321/0123456789abcdefghijklmn//firmware.bin",
    "https://192.168.044.8:49321/0123456789abcdefghijklmn/firmware.bin",
  };
  for (const char* url : invalid) {
    expect_true(!otaLocalHttpsUrlValid(url, OTA_URL_FIRMWARE),
                "arbitrary/non-normalized/non-private URL must fail");
  }
  expect_true(!otaLocalHttpsUrlValid(VALID_URL, OTA_URL_MANIFEST),
              "resource name must be bound to its role");

  char maxToken[OTA_TOKEN_MAX_BYTES];
  memset(maxToken, 't', sizeof(maxToken) - 1);
  maxToken[sizeof(maxToken) - 1] = 0;
  char maxTokenUrl[OTA_URL_MAX_BYTES];
  snprintf(maxTokenUrl, sizeof(maxTokenUrl),
           "https://10.0.0.1:1/%s/firmware.bin", maxToken);
  expect_true(otaLocalHttpsUrlValid(maxTokenUrl, OTA_URL_FIRMWARE),
              "127-byte token boundary should pass");
  char oversizedToken[OTA_TOKEN_MAX_BYTES + 1];
  memset(oversizedToken, 't', sizeof(oversizedToken) - 1);
  oversizedToken[sizeof(oversizedToken) - 1] = 0;
  char oversizedTokenUrl[OTA_URL_MAX_BYTES];
  snprintf(oversizedTokenUrl, sizeof(oversizedTokenUrl),
           "https://10.0.0.1:1/%s/firmware.bin", oversizedToken);
  expect_true(!otaLocalHttpsUrlValid(oversizedTokenUrl, OTA_URL_FIRMWARE),
              "128-byte token must fail");

  OtaOfferState offer = otaOfferStateInitial();
  expect_true(!otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 1000, true, false, false, false, true, 0, &offer),
              "offer outside a physical receive window must fail");
  expect_true(!otaOfferOpenReceiveWindow(&offer, 1000, false),
              "non-physical callers must not open the receive window");
  expect_true(otaOfferOpenReceiveWindow(&offer, 1000, true),
              "physical settings action should open a bounded window");
  expect_true(!otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 1050, true, false, false, false, false, 49, &offer),
              "receive-time battery gate should reject and clear a low-power hint");
  expect_true(otaOfferOpenReceiveWindow(&offer, 1060, true),
              "physical action should reopen after a power-gate rejection");
  expect_true(otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 1100, true, false, false, false, true, 0, &offer),
              "valid hint inside the physical window should be retained");
  expect_true(offer.pending && strcmp(offer.manifestUrl, VALID_MANIFEST_URL) == 0,
              "pending state should retain only bounded endpoint hints");
  OtaManifestDescriptor descriptor = {};
  char raw[OTA_MANIFEST_MAX_BYTES + 1];
  canonicalManifest(raw, sizeof(raw));
  expect_true(parse(raw, &descriptor) == OTA_MANIFEST_OK &&
              otaManifestMatchesOffer(descriptor, offer),
              "signed artifact must bind to the hinted endpoint/token");
  uint32_t offeredSize = offer.sizeBytes;
  offer.sizeBytes = offeredSize + 1;
  expect_true(!otaManifestMatchesOffer(descriptor, offer),
              "signed size must exactly match the physically accepted hint");
  offer.sizeBytes = offeredSize;
  char offeredVersion[OTA_VERSION_MAX_BYTES];
  memcpy(offeredVersion, offer.version, sizeof(offeredVersion));
  strcpy(offer.version, "1.2.4");
  expect_true(!otaManifestMatchesOffer(descriptor, offer),
              "signed version must exactly match the physically accepted hint");
  memcpy(offer.version, offeredVersion, sizeof(offer.version));

  expect_true(!otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL,
                "https://192.168.44.8:49321/differentabcdefghijkl/manifest.sig",
                "1.2.2", 1200, true, false, false, false, true, 0, &offer) &&
              !offer.pending && !offer.windowOpen,
              "bad subsequent hint must clear the prior offer and window");

  expect_true(otaOfferOpenReceiveWindow(&offer, 2000, true),
              "window should be reopenable by another physical action");
  expect_true(otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 2100, true, false, false, false, true, 0, &offer),
              "fresh offer should be accepted");
  otaOfferLifecyclePoll(&offer, 2101, false, false, false, false);
  expect_true(!offer.pending && !offer.windowOpen,
              "BLE disconnect must clear offer state");

  expect_true(otaOfferOpenReceiveWindow(&offer, 3000, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 3100, true, false, false, false, true, 0, &offer),
              "offer should be ready for conflict tests");
  otaOfferLifecyclePoll(&offer, 3101, true, true, false, false);
  expect_true(!offer.pending, "prompt conflict must clear pending offer");

  expect_true(otaOfferOpenReceiveWindow(&offer, 3200, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 3210, true, false, false, false, true, 0, &offer),
              "offer should be ready for transfer conflict test");
  otaOfferLifecyclePoll(&offer, 3211, true, false, true, false);
  expect_true(!offer.pending, "transfer conflict must clear pending offer");

  expect_true(otaOfferOpenReceiveWindow(&offer, 3300, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 3310, true, false, false, false, true, 0, &offer),
              "offer should be ready for other conflict test");
  otaOfferLifecyclePoll(&offer, 3311, true, false, false, true);
  expect_true(!offer.pending, "other functional conflict must clear pending offer");

  expect_true(otaOfferOpenReceiveWindow(&offer, 4000, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 4100, true, false, false, false, true, 0, &offer),
              "offer should be ready for expiry test");
  otaOfferLifecyclePoll(
    &offer, 4100 + OTA_OFFER_TTL_MS + 1, true, false, false, false
  );
  expect_true(!offer.pending && !offer.windowOpen,
              "offer expiry must clear all retained hints");

  expect_true(otaOfferOpenReceiveWindow(&offer, 5000, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 5100, true, false, false, false, true, 0, &offer),
              "offer should be ready for execution recheck");
  expect_true(otaOfferExecutionAllowed(
                &offer, 5200, true, false, false, false, false, 50),
              "execution recheck should accept a conflict-free 50 percent battery");
  OtaOfferState consumed = otaOfferStateInitial();
  expect_true(otaOfferConsume(&offer, &consumed) && consumed.pending &&
                strcmp(consumed.manifestUrl, VALID_MANIFEST_URL) == 0 &&
                !offer.pending && !offer.windowOpen,
              "physical acceptance should move the hint into private runtime state");

  expect_true(otaOfferOpenReceiveWindow(&offer, 5210, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 5211, true, false, false, false, true, 0, &offer),
              "offer should be recreated for a failed execution recheck");
  expect_true(!otaOfferExecutionAllowed(
                &offer, 5212, true, false, false, false, false, 49) &&
              !offer.pending,
              "execution recheck must clear offer when power gate fails");

  expect_true(otaOfferOpenReceiveWindow(&offer, 5300, true) &&
              otaOfferAcceptHint(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", 5310, true, false, false, false, true, 0, &offer),
              "offer should be ready for execution conflict recheck");
  expect_true(!otaOfferExecutionAllowed(
                &offer, 5320, true, true, false, false, true, 100) &&
              !offer.pending,
              "execution recheck must clear offer when a prompt appears");

  expect_true(otaOfferOpenReceiveWindow(&offer, 6000, true),
              "physical action should open final cancellation window");
  otaOfferCancel(&offer);
  expect_true(!offer.pending && !offer.windowOpen,
              "cancel or reject must clear all offer state");
}

int main() {
  expect_true(strcmp(CODE_BUDDY_FIRMWARE_VERSION, "0.1.4") == 0,
              "firmware must expose an explicit semantic current version");
  testExactCanonicalManifestAndDetachedDescriptor();
  testAuthenticationAlwaysPrecedesParsing();
  testCanonicalBytesRejectAmbiguity();
  testTypedFieldsAndMonotonicVersion();
  testMacLocalUrlPolicyAndBinding();
  return 0;
}
