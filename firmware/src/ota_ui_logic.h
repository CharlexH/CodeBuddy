#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct OtaUiPlan {
  bool showInstall;
  bool showCancel;
  bool readOnly;
  bool compactOverlay;
};

struct OtaCompactOverlayLayout {
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t height;
};

inline OtaUiPlan otaUiPlan(
  bool visible,
  bool confirm,
  bool automatic,
  bool irreversible,
  bool cancellable
) {
  OtaUiPlan plan = {};
  if (!visible) return plan;
  plan.compactOverlay = automatic;
  plan.readOnly = irreversible || !cancellable;
  plan.showInstall = confirm && !automatic && cancellable && !irreversible;
  plan.showCancel = cancellable && !irreversible;
  return plan;
}

inline constexpr OtaCompactOverlayLayout otaCompactOverlayLayout(
  bool landscape
) {
  return landscape
    ? OtaCompactOverlayLayout{40, 45, 160, 44}
    : OtaCompactOverlayLayout{8, 94, 119, 52};
}

inline void otaFormatReadableSize(
  uint32_t bytes, char* output, size_t outputSize
) {
  if (!output || !outputSize) return;
  if (bytes < 1024) {
    std::snprintf(output, outputSize, "%lu B", static_cast<unsigned long>(bytes));
    return;
  }
  const uint32_t unit = bytes < 1024UL * 1024UL ? 1024UL : 1024UL * 1024UL;
  const char* suffix = unit == 1024UL ? "KB" : "MB";
  uint32_t tenths = static_cast<uint32_t>(
    (static_cast<uint64_t>(bytes) * 10ULL + unit / 2) / unit
  );
  std::snprintf(
    output, outputSize, "%lu.%lu %s",
    static_cast<unsigned long>(tenths / 10),
    static_cast<unsigned long>(tenths % 10), suffix
  );
}
