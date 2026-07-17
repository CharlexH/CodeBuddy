#include <assert.h>

#include <ArduinoJson.h>
#include "status_dashboard_json.h"

static void applyUnreadJson(const char* payload, StatusDashboardCounts* counts) {
  JsonDocument doc;
  assert(!deserializeJson(doc, payload));
  statusDashboardApplyUnreadJson(doc["unread"], counts);
}

int main() {
  StatusDashboardCounts counts = {};
  applyUnreadJson("{}", &counts);
  assert(!counts.hasUnread);

  applyUnreadJson("{\"unread\":7}", &counts);
  assert(counts.hasUnread && counts.unread == 7);

  applyUnreadJson("{}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":null}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":7.5}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":\"8\"}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":-1}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":256}", &counts);
  assert(counts.unread == 7);
  applyUnreadJson("{\"unread\":0}", &counts);
  assert(counts.hasUnread && counts.unread == 0);
  return 0;
}
