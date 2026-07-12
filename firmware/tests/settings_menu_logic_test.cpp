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

  return 0;
}
