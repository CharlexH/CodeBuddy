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

  assert(STATUS_DASHBOARD_LABEL_TEXT_SIZE == 2);
  assert(STATUS_DASHBOARD_LABEL_CENTER_Y == 11);
  assert(STATUS_DASHBOARD_COUNT_CENTER_Y == 40);
  assert(statusDashboardCountTextSize(0) == 3);
  assert(statusDashboardCountTextSize(99) == 3);
  assert(statusDashboardCountTextSize(100) == 2);

  assert(statusDashboardColorRole(STATUS_RUN, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_ASK, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_NEW, 0) == STATUS_COLOR_DIM);
  assert(statusDashboardColorRole(STATUS_RUN, 1) == STATUS_COLOR_GREEN);
  assert(statusDashboardColorRole(STATUS_ASK, 1) == STATUS_COLOR_AMBER);
  assert(statusDashboardColorRole(STATUS_NEW, 1) == STATUS_COLOR_CYAN);
  return 0;
}
