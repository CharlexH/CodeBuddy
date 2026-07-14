#include <cstdio>
#include <cstring>

#include "../src/ota_ui_logic.h"

static int expect(bool value, int code) { return value ? 0 : code; }

int main() {
  OtaUiPlan confirm = otaUiPlan(true, true, false, false, true);
  if (int code = expect(confirm.showInstall && confirm.showCancel && !confirm.readOnly &&
                        !confirm.compactOverlay, 1)) return code;
  OtaUiPlan direct = otaUiPlan(true, true, true, false, true);
  if (int code = expect(!direct.showInstall && direct.showCancel && !direct.readOnly &&
                        direct.compactOverlay, 6)) return code;
  OtaUiPlan download = otaUiPlan(true, false, true, false, true);
  if (int code = expect(!download.showInstall && download.showCancel && !download.readOnly &&
                        download.compactOverlay, 2)) return code;
  OtaUiPlan committed = otaUiPlan(true, false, true, true, false);
  if (int code = expect(!committed.showInstall && !committed.showCancel && committed.readOnly &&
                        committed.compactOverlay, 3)) return code;
  OtaUiPlan hidden = otaUiPlan(false, false, true, false, true);
  if (int code = expect(!hidden.compactOverlay, 7)) return code;

  OtaCompactOverlayLayout portrait = otaCompactOverlayLayout(false);
  if (int code = expect(
        portrait.x == 8 && portrait.y == 94 &&
        portrait.width == 119 && portrait.height == 52,
        8
      )) return code;
  OtaCompactOverlayLayout landscape = otaCompactOverlayLayout(true);
  if (int code = expect(
        landscape.x == 40 && landscape.y == 45 &&
        landscape.width == 160 && landscape.height == 44,
        9
      )) return code;
  char size[16] = {};
  otaFormatReadableSize(1234, size, sizeof(size));
  if (std::strcmp(size, "1.2 KB") != 0) return 4;
  otaFormatReadableSize(3 * 1024 * 1024, size, sizeof(size));
  if (std::strcmp(size, "3.0 MB") != 0) return 5;
  return 0;
}
