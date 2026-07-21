#include <assert.h>
#include <string.h>

#include "landscape_dashboard_logic.h"

int main() {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  assert(layout.screenWidth == 240 && layout.screenHeight == 135);
  assert(layout.statusX == 4 && layout.statusY == 4);
  assert(layout.heartbeatX == 172 && layout.heartbeatY == 4);
  assert(layout.timeX == 4 && layout.timeY == 34);
  assert(layout.secondsX == 129 && layout.secondsY == 44);
  assert(layout.dateY == 79);
  assert(layout.cardsX == 172 && layout.cardsWidth == 64);
  assert(layout.cardsY[0] == 34 && layout.cardsY[1] == 57 && layout.cardsY[2] == 79);
  assert(layout.meterY == 109 && layout.meterFootprint == 26);
  assert(landscapeDashboardStatus(false, 3, 2) == DASHBOARD_OFFLINE);
  assert(landscapeDashboardStatus(true, 3, 2) == DASHBOARD_WAITING);
  assert(landscapeDashboardStatus(true, 3, 0) == DASHBOARD_RUNNING);
  assert(landscapeDashboardStatus(true, 0, 0) == DASHBOARD_IDLE);
  assert(strcmp(landscapeDashboardStatusLabel(DASHBOARD_RUNNING), "RUNNING") == 0);
  assert(strcmp(landscapeDashboardStatusLabel(DASHBOARD_WAITING), "WAITING") == 0);
  assert(strcmp(landscapeDashboardStatusLabel(DASHBOARD_IDLE), "IDLE") == 0);
  assert(strcmp(landscapeDashboardStatusLabel(DASHBOARD_OFFLINE), "OFFLINE") == 0);

  assert(landscapeDashboardSecondBlocks(-1) == 0);
  assert(landscapeDashboardSecondBlocks(0) == 0);
  assert(landscapeDashboardSecondBlocks(1) == 1);
  assert(landscapeDashboardSecondBlocks(15) == 1);
  assert(landscapeDashboardSecondBlocks(30) == 2);
  assert(landscapeDashboardSecondBlocks(45) == 3);
  assert(landscapeDashboardSecondBlocks(59) == 4);
  assert(landscapeDashboardSecondBlocks(60) == 0);

  assert(landscapeDashboardActivityMaskAt(0x1, 1000, 1000) == 0x1);
  assert(landscapeDashboardActivityMaskAt(0x1, 1000, 2999) == 0x2);
  assert(landscapeDashboardActivityMaskAt(0x1, 1000, 21000) == 0);
  assert(landscapeDashboardActivityVisibleAt(1UL << 19, 0));
  assert(landscapeDashboardActivityVisibleAt(1UL, 19));
  assert(!landscapeDashboardActivityVisibleAt(0, 10));
  assert(landscapeDashboardHeartbeatHeight(0) == 4);
  assert(landscapeDashboardHeartbeatHeight(2) == 12);
  assert(landscapeDashboardHeartbeatHeight(19) >= 4);
  return 0;
}
