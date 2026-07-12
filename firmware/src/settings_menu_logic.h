#pragma once

#include <stdint.h>

enum SettingsMenuAction : uint8_t {
  SETTINGS_BRIGHTNESS,
  SETTINGS_SOUND,
  SETTINGS_BLUETOOTH,
  SETTINGS_WIFI,
  SETTINGS_LED,
  SETTINGS_CLOCK_ROTATION,
  SETTINGS_PET,
  SETTINGS_RESET,
  SETTINGS_BACK,
  SETTINGS_INVALID,
};

inline constexpr uint8_t settingsMenuItemCount() { return 9; }

inline constexpr SettingsMenuAction settingsMenuAction(uint8_t index) {
  return index < settingsMenuItemCount()
      ? static_cast<SettingsMenuAction>(index)
      : SETTINGS_INVALID;
}

inline constexpr const char* settingsMenuLabel(uint8_t index) {
  return index == SETTINGS_BRIGHTNESS ? "brightness"
      : index == SETTINGS_SOUND ? "sound"
      : index == SETTINGS_BLUETOOTH ? "bluetooth"
      : index == SETTINGS_WIFI ? "wifi"
      : index == SETTINGS_LED ? "led"
      : index == SETTINGS_CLOCK_ROTATION ? "clock rot"
      : index == SETTINGS_PET ? "ascii pet"
      : index == SETTINGS_RESET ? "reset"
      : index == SETTINGS_BACK ? "back"
      : "";
}
