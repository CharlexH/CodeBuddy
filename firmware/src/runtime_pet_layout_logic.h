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

struct RuntimeGifPlacement {
  int16_t x;
  int16_t y;
  uint8_t scaleDivisor;
};

struct RuntimePetClearRect {
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
};

inline constexpr RuntimePetLayout runtimePetLayout(bool landscape) {
  return landscape ? RuntimePetLayout{240, 119, 120, 59, 1, 18}
                   : RuntimePetLayout{135, 224, 67, 112, 2, 30};
}

inline constexpr RuntimePetClearRect runtimePetClearRect(bool landscape) {
  return landscape ? RuntimePetClearRect{0, 0, 240, 119}
                   : RuntimePetClearRect{0, 0, 135, 224};
}

inline constexpr uint8_t runtimeGifScaleDivisor(
  bool landscape,
  uint16_t gifWidth,
  uint16_t gifHeight
) {
  return landscape && (gifWidth > 236 || gifHeight > 119) ? 2 : 1;
}

inline RuntimeGifPlacement runtimeGifPlacement(
  bool landscape,
  uint16_t gifWidth,
  uint16_t gifHeight
) {
  RuntimePetLayout layout = runtimePetLayout(landscape);
  uint8_t divisor = runtimeGifScaleDivisor(landscape, gifWidth, gifHeight);
  int16_t outputWidth = gifWidth / divisor;
  int16_t outputHeight = gifHeight / divisor;
  return RuntimeGifPlacement{
    (int16_t)(layout.centerX - outputWidth / 2),
    (int16_t)(layout.centerY - outputHeight / 2),
    divisor,
  };
}

inline constexpr bool runtimeStatusOverlayVisible(bool inPrompt) {
  return inPrompt;
}

inline constexpr bool runtimeNeedsFullRepaintOnPromptExit(bool wasPrompt, bool inPrompt) {
  return wasPrompt && !inPrompt;
}
