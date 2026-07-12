#include "ota_manifest.h"

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#include "firmware_version.h"
#include "ota_trust.h"

namespace {

bool verifyDetachedP256Sha256(
  const uint8_t* manifest,
  size_t manifestLength,
  const uint8_t* signature,
  size_t signatureLength,
  void*
) {
#if !CODE_BUDDY_OTA_ENABLED
  (void)manifest;
  (void)manifestLength;
  (void)signature;
  (void)signatureLength;
  return false;
#else
  mbedtls_pk_context publicKey;
  mbedtls_pk_init(&publicKey);
  uint8_t digest[32] = {};
  const char* publicPem = otaTrustManifestPublicKeyPem();
  int result = mbedtls_pk_parse_public_key(
    &publicKey,
    reinterpret_cast<const uint8_t*>(publicPem),
    strlen(publicPem) + 1
  );
  if (result == 0 &&
      (!mbedtls_pk_can_do(&publicKey, MBEDTLS_PK_ECDSA) ||
       mbedtls_pk_get_bitlen(&publicKey) != 256)) result = MBEDTLS_ERR_PK_TYPE_MISMATCH;
  if (result == 0) result = mbedtls_sha256_ret(manifest, manifestLength, digest, 0);
  if (result == 0) {
    result = mbedtls_pk_verify(
      &publicKey,
      MBEDTLS_MD_SHA256,
      digest,
      sizeof(digest),
      signature,
      signatureLength
    );
  }
  mbedtls_platform_zeroize(digest, sizeof(digest));
  mbedtls_pk_free(&publicKey);
  return result == 0;
#endif
}

}  // namespace

OtaManifestResult otaManifestAuthenticateAndParse(
  const uint8_t* rawManifest,
  size_t rawManifestLength,
  const uint8_t* derSignature,
  size_t derSignatureLength,
  OtaManifestDescriptor* descriptor
) {
  return otaManifestAuthenticateWith(
    rawManifest,
    rawManifestLength,
    derSignature,
    derSignatureLength,
    CODE_BUDDY_FIRMWARE_VERSION,
    verifyDetachedP256Sha256,
    nullptr,
    descriptor
  );
}
