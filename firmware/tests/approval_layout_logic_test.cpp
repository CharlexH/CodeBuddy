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
  const PortraitApprovalLayout withoutMeter = portraitApprovalLayout(false);
  expect_true(withoutMeter.hintStartY == 194,
              "approval without a meter should preserve the existing hint position");
  expect_true(withoutMeter.maxHintRows == 2,
              "approval without a meter should preserve two visible hint rows");
  expect_true(withoutMeter.footerY == 228,
              "approval without a meter should preserve the existing footer position");
  expect_true(portraitApprovalHintBottom(withoutMeter) <= withoutMeter.footerY - 4,
              "approval hints should keep a defined gap above the footer");

  const PortraitApprovalLayout withMeter = portraitApprovalLayout(true);
  expect_true(withMeter.hintStartY == 194,
              "meter-visible approval should keep the first hint row stable");
  expect_true(withMeter.maxHintRows == 1,
              "meter-visible approval should show only one concise hint row");
  expect_true(withMeter.footerY == 212,
              "meter-visible approval should lift the footer above the meter");
  expect_true(portraitApprovalHintBottom(withMeter) <= withMeter.footerY - 4,
              "meter-visible hints must not overlap the lifted footer");

  return 0;
}
