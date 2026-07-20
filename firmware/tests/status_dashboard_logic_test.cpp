#include <assert.h>
#include <string.h>

#include "status_dashboard_logic.h"

int main() {
  StatusDashboardCounts counts = {};
  statusDashboardApplyUnread(&counts, true, true, 7);
  assert(counts.hasUnread);
  assert(counts.unread == 7);

  statusDashboardApplyUnread(&counts, false, false, 0);
  assert(counts.unread == 7);

  statusDashboardApplyUnread(&counts, true, false, 300);
  assert(counts.unread == 7);

  char text[4] = {};
  statusDashboardFormatCount(text, sizeof(text), 0);
  assert(strcmp(text, "0") == 0);
  statusDashboardFormatCount(text, sizeof(text), 99);
  assert(strcmp(text, "99") == 0);
  statusDashboardFormatCount(text, sizeof(text), 100);
  assert(strcmp(text, "99+") == 0);
  statusDashboardFormatCount(text, sizeof(text), 255);
  assert(strcmp(text, "99+") == 0);

  assert(STATUS_DASHBOARD_LABEL_TEXT_SIZE == 0.57f);
  assert(STATUS_DASHBOARD_LABEL_CENTER_Y == 11);
  assert(STATUS_DASHBOARD_COUNT_CENTER_Y == 40);
  assert(statusDashboardLabelCenterY(4) == 15);
  assert(statusDashboardCountCenterY(4) == 44);
  assert(STATUS_DASHBOARD_COUNT_TEXT_SIZE == 0.76f);
  assert(statusDashboardAsciiTextWidth(3, STATUS_DASHBOARD_LABEL_TEXT_SIZE) <= 40);
  assert(statusDashboardAsciiTextWidth(3, STATUS_DASHBOARD_COUNT_TEXT_SIZE) <= 40);
  assert(statusDashboardAsciiTextHeight(STATUS_DASHBOARD_LABEL_TEXT_SIZE) <= 12);
  assert(statusDashboardAsciiTextHeight(STATUS_DASHBOARD_COUNT_TEXT_SIZE) <= 16);

  assert(statusDashboardColorRole(STATUS_RUN, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_ASK, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_NEW, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_RUN, 1) == STATUS_COLOR_GREEN);
  assert(statusDashboardColorRole(STATUS_ASK, 1) == STATUS_COLOR_AMBER);
  assert(statusDashboardColorRole(STATUS_NEW, 1) == STATUS_COLOR_CYAN);
  return 0;
}
