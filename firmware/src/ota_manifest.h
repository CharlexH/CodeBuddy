#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ota_manifest_logic.h"

bool otaVerifyDetachedSignature(
  const uint8_t* payload,
  size_t payloadLength,
  const uint8_t* derSignature,
  size_t derSignatureLength,
  void* context = nullptr
);

OtaManifestResult otaManifestAuthenticateAndParse(
  const uint8_t* rawManifest,
  size_t rawManifestLength,
  const uint8_t* derSignature,
  size_t derSignatureLength,
  OtaManifestDescriptor* descriptor
);
