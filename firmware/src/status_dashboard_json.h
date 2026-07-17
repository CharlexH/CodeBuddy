#pragma once

#include <ArduinoJson.h>

#include "status_dashboard_logic.h"

inline void statusDashboardApplyUnreadJson(
  JsonVariantConst unreadVariant,
  StatusDashboardCounts* counts
) {
  const bool fieldPresent = !unreadVariant.isUnbound();
  const bool fieldValid = fieldPresent && unreadVariant.is<int>() &&
    !unreadVariant.is<bool>();
  const int unread = fieldValid ? unreadVariant.as<int>() : 0;
  statusDashboardApplyUnread(counts, fieldPresent, fieldValid, unread);
}
