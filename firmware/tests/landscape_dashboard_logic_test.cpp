#include <assert.h>
#include <string.h>

#include "landscape_dashboard_logic.h"

struct RecordedHeartbeatLine {
  int16_t x0;
  int16_t y0;
  int16_t x1;
  int16_t y1;
};

struct HeartbeatCanvas {
  RecordedHeartbeatLine lines[128] = {};
  uint8_t count = 0;

  void drawSmoothLine(
    int16_t x0,
    int16_t y0,
    int16_t x1,
    int16_t y1,
    uint16_t
  ) {
    assert(count < 128);
    lines[count++] = {x0, y0, x1, y1};
  }
};

int main() {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  assert(layout.screenWidth == 240 && layout.screenHeight == 135);
  assert(layout.statusX == 4 && layout.statusY == 4);
  assert(layout.heartbeatX == 172 && layout.heartbeatY == 4);
  assert(layout.timeX == 4 && layout.timeY == 34);
  assert(layout.secondsX == 129 && layout.secondsY == 48);
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
  assert(LANDSCAPE_DASHBOARD_SECONDS_TEXT == 0xA514);
  assert(LANDSCAPE_DASHBOARD_DATE_TEXT == LANDSCAPE_DASHBOARD_IDLE);
  assert(
    landscapeDashboardCountColor(0, LANDSCAPE_DASHBOARD_RUN) ==
    LANDSCAPE_DASHBOARD_WHITE_40
  );
  assert(
    landscapeDashboardCountColor(1, LANDSCAPE_DASHBOARD_RUN) ==
    LANDSCAPE_DASHBOARD_RUN
  );

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
  HeartbeatCanvas emptyCurve;
  landscapeDashboardDrawHeartbeatCurve(
    emptyCurve, 0, layout.heartbeatX, layout.heartbeatCenterY,
    LANDSCAPE_DASHBOARD_GREEN
  );
  assert(emptyCurve.count == 0);

  HeartbeatCanvas activeCurve;
  const uint32_t adjacentActivity = (1UL << (19 - 9)) | (1UL << (19 - 10));
  landscapeDashboardDrawHeartbeatCurve(
    activeCurve, adjacentActivity, layout.heartbeatX,
    layout.heartbeatCenterY, LANDSCAPE_DASHBOARD_GREEN
  );
  assert(activeCurve.count > 8);
  bool risesAboveBaseline = false;
  bool fallsBelowBaseline = false;
  for (uint8_t i = 0; i < activeCurve.count; ++i) {
    const RecordedHeartbeatLine& line = activeCurve.lines[i];
    assert(line.x0 >= layout.heartbeatX);
    assert(line.x1 < layout.heartbeatX + layout.heartbeatWidth);
    assert(line.y0 >= layout.heartbeatY);
    assert(line.y1 < layout.heartbeatY + layout.heartbeatHeight);
    risesAboveBaseline |= line.y0 < layout.heartbeatCenterY ||
      line.y1 < layout.heartbeatCenterY;
    fallsBelowBaseline |= line.y0 > layout.heartbeatCenterY ||
      line.y1 > layout.heartbeatCenterY;
    if (i > 0) {
      assert(activeCurve.lines[i - 1].x1 == line.x0);
      assert(activeCurve.lines[i - 1].y1 == line.y0);
    }
  }
  assert(risesAboveBaseline && fallsBelowBaseline);
  return 0;
}
