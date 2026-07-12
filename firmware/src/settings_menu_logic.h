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
  SETTINGS_OTA_UPDATE,
  SETTINGS_RESET,
  SETTINGS_BACK,
  SETTINGS_INVALID,
};

enum WifiMenuAction : uint8_t {
  WIFI_MENU_STATUS,
  WIFI_MENU_CHANGE,
  WIFI_MENU_FORGET,
  WIFI_MENU_SETUP,
  WIFI_MENU_BACK,
  WIFI_MENU_INVALID,
};

inline constexpr uint8_t settingsMenuItemCount() { return 10; }

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
      : index == SETTINGS_OTA_UPDATE ? "ota update"
      : index == SETTINGS_RESET ? "reset"
      : index == SETTINGS_BACK ? "back"
      : "";
}

inline constexpr uint8_t wifiMenuItemCount(bool provisioned) {
  return provisioned ? 4 : 2;
}

inline constexpr WifiMenuAction wifiMenuAction(bool provisioned, uint8_t index) {
  return provisioned
      ? (index == 0 ? WIFI_MENU_STATUS
          : index == 1 ? WIFI_MENU_CHANGE
          : index == 2 ? WIFI_MENU_FORGET
          : index == 3 ? WIFI_MENU_BACK
          : WIFI_MENU_INVALID)
      : (index == 0 ? WIFI_MENU_SETUP
          : index == 1 ? WIFI_MENU_BACK
          : WIFI_MENU_INVALID);
}

inline const char* wifiMenuLabel(bool provisioned, uint8_t index) {
  WifiMenuAction action = wifiMenuAction(provisioned, index);
  return action == WIFI_MENU_STATUS ? "status"
      : action == WIFI_MENU_CHANGE ? "change wifi"
      : action == WIFI_MENU_FORGET ? "forget wifi"
      : action == WIFI_MENU_SETUP ? "setup"
      : action == WIFI_MENU_BACK ? "back"
      : "";
}
