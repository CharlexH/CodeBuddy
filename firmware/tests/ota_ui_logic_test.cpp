#include <cstdio>
#include <cstring>

#include "../src/ota_ui_logic.h"

static int expect(bool value, int code) { return value ? 0 : code; }

int main() {
  OtaUiPlan confirm = otaUiPlan(true, true, false, true);
  if (int code = expect(confirm.showInstall && confirm.showCancel && !confirm.readOnly, 1)) return code;
  OtaUiPlan download = otaUiPlan(true, false, false, true);
  if (int code = expect(!download.showInstall && download.showCancel && !download.readOnly, 2)) return code;
  OtaUiPlan committed = otaUiPlan(true, false, true, false);
  if (int code = expect(!committed.showInstall && !committed.showCancel && committed.readOnly, 3)) return code;
  char size[16] = {};
  otaFormatReadableSize(1234, size, sizeof(size));
  if (std::strcmp(size, "1.2 KB") != 0) return 4;
  otaFormatReadableSize(3 * 1024 * 1024, size, sizeof(size));
  if (std::strcmp(size, "3.0 MB") != 0) return 5;
  return 0;
}
