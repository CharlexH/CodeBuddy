#pragma once

#include <stddef.h>
#include <stdint.h>
#include "ota_manifest_logic.h"

OtaManifestResult otaManifestAuthenticateAndParse(
  const uint8_t* rawManifest,
  size_t rawManifestLength,
  const uint8_t* derSignature,
  size_t derSignatureLength,
  OtaManifestDescriptor* descriptor
);
