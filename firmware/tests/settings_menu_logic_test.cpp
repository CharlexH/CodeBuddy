#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings_menu_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  const SettingsMenuAction expected[] = {
    SETTINGS_BRIGHTNESS,
    SETTINGS_SOUND,
    SETTINGS_BLUETOOTH,
    SETTINGS_WIFI,
    SETTINGS_LED,
    SETTINGS_CLOCK_ROTATION,
    SETTINGS_PET,
    SETTINGS_RESET,
    SETTINGS_BACK,
  };
  const char* labels[] = {
    "brightness",
    "sound",
    "bluetooth",
    "wifi",
    "led",
    "clock rot",
    "ascii pet",
    "reset",
    "back",
  };

  expect_true(settingsMenuItemCount() == 9,
              "settings should expose nine rows after removing transcript");
  for (uint8_t i = 0; i < settingsMenuItemCount(); ++i) {
    expect_true(settingsMenuAction(i) == expected[i],
                "settings actions should retain their intended order");
    expect_true(strcmp(settingsMenuLabel(i), labels[i]) == 0,
                "settings labels should match the pet-only menu");
  }
  expect_true(settingsMenuAction(99) == SETTINGS_INVALID,
              "out-of-range settings rows should not dispatch an action");
  expect_true(strcmp(settingsMenuLabel(99), "") == 0,
              "out-of-range settings rows should have no visible label");

  expect_true(wifiMenuItemCount(false) == 2,
              "unprovisioned Wi-Fi menu should expose setup and back");
  expect_true(wifiMenuAction(false, 0) == WIFI_MENU_SETUP,
              "unprovisioned Wi-Fi should start setup clearly");
  expect_true(strcmp(wifiMenuLabel(false, 0), "setup") == 0,
              "setup row should be explicit");
  expect_true(wifiMenuItemCount(true) == 4,
              "provisioned Wi-Fi should expose status/change/forget/back");
  const WifiMenuAction wifiExpected[] = {
    WIFI_MENU_STATUS, WIFI_MENU_CHANGE, WIFI_MENU_FORGET, WIFI_MENU_BACK,
  };
  const char* wifiLabels[] = {"status", "change wifi", "forget wifi", "back"};
  for (uint8_t i = 0; i < wifiMenuItemCount(true); ++i) {
    expect_true(wifiMenuAction(true, i) == wifiExpected[i],
                "Wi-Fi submenu actions should retain their intended order");
    expect_true(strcmp(wifiMenuLabel(true, i), wifiLabels[i]) == 0,
                "Wi-Fi submenu labels should be stable");
  }

  return 0;
}
