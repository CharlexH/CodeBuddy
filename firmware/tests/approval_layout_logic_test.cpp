#include <stdio.h>
#include <stdlib.h>

#include "approval_layout_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  const PortraitApprovalLayout withoutMeter = portraitApprovalLayout(0);
  expect_true(withoutMeter.hintStartY == 194,
              "approval without a meter should preserve the existing hint position");
  expect_true(withoutMeter.maxHintRows == 2,
              "approval without a meter should preserve two visible hint rows");
  expect_true(withoutMeter.footerY == 228,
              "approval without a meter should preserve the existing footer position");
  expect_true(portraitApprovalHintBottom(withoutMeter) <= withoutMeter.footerY - 4,
              "approval hints should keep a defined gap above the footer");

  const PortraitApprovalLayout withSingleMeter = portraitApprovalLayout(8);
  expect_true(withSingleMeter.hintStartY == 194,
              "single-meter approval should keep the first hint row stable");
  expect_true(withSingleMeter.maxHintRows == 1,
              "single-meter approval should show one concise hint row");
  expect_true(withSingleMeter.footerY == 220,
              "single-meter approval should reserve exactly eight footer pixels");
  expect_true(portraitApprovalHintBottom(withSingleMeter) <= withSingleMeter.footerY - 4,
              "single-meter hints must not overlap the lifted footer");

  const PortraitApprovalLayout withDualMeter = portraitApprovalLayout(16);
  expect_true(withDualMeter.hintStartY == 194,
              "dual-meter approval should keep the first hint row stable");
  expect_true(withDualMeter.maxHintRows == 1,
              "dual-meter approval should show one concise hint row");
  expect_true(withDualMeter.footerY == 212,
              "dual-meter approval should reserve exactly sixteen footer pixels");
  expect_true(portraitApprovalHintBottom(withDualMeter) <= withDualMeter.footerY - 4,
              "dual-meter hints must not overlap the lifted footer");

  expect_true(landscapeApprovalFooterY(135, 0) == 123,
              "landscape approval without usage should preserve its footer position");
  expect_true(landscapeApprovalFooterY(135, 8) == 115,
              "landscape approval should reserve the single-meter footprint");
  expect_true(landscapeApprovalFooterY(135, 16) == 107,
              "landscape approval should reserve the dual-meter footprint");

  return 0;
}
