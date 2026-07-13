#pragma once

#include <stdint.h>

struct PortraitApprovalLayout {
  int16_t hintStartY;
  uint8_t maxHintRows;
  int16_t footerY;
  uint8_t hintLineHeight;
  uint8_t hintGlyphHeight;
};

inline constexpr PortraitApprovalLayout portraitApprovalLayout(uint8_t footerInset) {
  return PortraitApprovalLayout{
    194,
    (uint8_t)((footerInset > 16 ? 16 : footerInset) <= 6 ? 2 : 1),
    (int16_t)(228 - (footerInset > 16 ? 16 : footerInset)),
    12,
    12,
  };
}

inline constexpr int16_t portraitApprovalHintBottom(
  const PortraitApprovalLayout& layout
) {
  return layout.hintStartY
      + (layout.maxHintRows - 1) * layout.hintLineHeight
      + layout.hintGlyphHeight;
}

inline constexpr int16_t landscapeApprovalFooterY(
  uint16_t screenHeight,
  uint8_t footerInset
) {
  return (int16_t)screenHeight - 12 - (footerInset > 16 ? 16 : footerInset);
}
