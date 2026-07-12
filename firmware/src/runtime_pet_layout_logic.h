#pragma once

#include <stdint.h>

struct RuntimePetLayout {
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  int16_t centerX;
  int16_t centerY;
  uint8_t asciiScale;
  int16_t asciiYOffset;
};

inline constexpr RuntimePetLayout runtimePetLayout(bool landscape) {
  return landscape ? RuntimePetLayout{240, 119, 120, 59, 1, 18}
                   : RuntimePetLayout{135, 224, 67, 112, 2, 30};
}

inline constexpr bool runtimeStatusOverlayVisible(bool inPrompt) {
  return inPrompt;
}

inline constexpr bool runtimeNeedsFullRepaintOnPromptExit(bool wasPrompt, bool inPrompt) {
  return wasPrompt && !inPrompt;
}
