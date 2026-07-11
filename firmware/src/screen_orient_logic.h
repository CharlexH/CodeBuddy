#pragma once

inline bool screenOrientRuntimeEligible(
  bool normal_display,
  bool menu_open,
  bool settings_open,
  bool reset_open,
  bool codex_active,
  bool codex_waiting,
  bool approval_visible
) {
  return normal_display
      && !menu_open
      && !settings_open
      && !reset_open
      && (codex_active || codex_waiting || approval_visible);
}
