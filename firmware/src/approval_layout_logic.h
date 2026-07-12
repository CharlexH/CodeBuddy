#pragma once

#include <stdint.h>

struct PortraitApprovalLayout {
  int16_t hintStartY;
  uint8_t maxHintRows;
  int16_t footerY;
  uint8_t hintLineHeight;
  uint8_t hintGlyphHeight;
};

inline constexpr PortraitApprovalLayout portraitApprovalLayout(bool meterVisible) {
  return meterVisible
      ? PortraitApprovalLayout{194, 1, 212, 12, 12}
      : PortraitApprovalLayout{194, 2, 228, 12, 12};
}

inline constexpr int16_t portraitApprovalHintBottom(
  const PortraitApprovalLayout& layout
) {
  return layout.hintStartY
      + (layout.maxHintRows - 1) * layout.hintLineHeight
      + layout.hintGlyphHeight;
}
