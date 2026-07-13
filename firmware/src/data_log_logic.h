#pragma once

#include <cstddef>
#include <cstdio>

inline void formatMalformedJsonLog(
  size_t inputLength, char* output, size_t outputSize
) {
  if (!output || !outputSize) return;
  std::snprintf(
    output, outputSize, "[data] json malformed len=%lu",
    static_cast<unsigned long>(inputLength)
  );
}
