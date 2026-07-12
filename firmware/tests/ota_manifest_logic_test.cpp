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
    "1.2.3-", "1.2.3+", "1.2.3..4",
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
}

static void testMacLocalUrlPolicyAndBinding() {
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

  OtaOfferDescriptor offer = {};
  expect_true(otaOfferAccept(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", false, false, &offer),
              "valid offer should store fixed coordination metadata");
  expect_true(offer.pending && strcmp(offer.version, "1.2.3") == 0,
              "accepted offer should become pending");
  OtaManifestDescriptor descriptor = {};
  char raw[OTA_MANIFEST_MAX_BYTES + 1];
  canonicalManifest(raw, sizeof(raw));
  expect_true(parse(raw, &descriptor) == OTA_MANIFEST_OK &&
              otaManifestMatchesOffer(descriptor, offer),
              "signed artifact must bind to offer origin, token, version and size");

  OtaOfferDescriptor rejected = {};
  expect_true(!otaOfferAccept(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", true, false, &rejected) && !rejected.pending,
              "approval conflict must prevent storing an offer");
  expect_true(!otaOfferAccept(
                "1.2.3", 1234, VALID_MANIFEST_URL, VALID_SIGNATURE_URL,
                "1.2.2", false, true, &rejected) && !rejected.pending,
              "transfer conflict must prevent storing an offer");
  expect_true(!otaOfferAccept(
                "1.2.3", 1234, VALID_MANIFEST_URL,
                "https://192.168.44.8:49321/differentabcdefghijkl/manifest.sig",
                "1.2.2", false, false, &rejected),
              "manifest and signature URLs must share exact endpoint/token");
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
