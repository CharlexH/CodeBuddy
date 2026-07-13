#include "board_compat.h"
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "about_info.h"
#include "approval_layout_logic.h"
#include "clock_display_logic.h"
#include "clock_orient_logic.h"
#include "screen_orient_logic.h"
#include "runtime_pet_layout_logic.h"
#include "settings_menu_logic.h"
#include "wifi_manager.h"
#include "ota_update.h"
#include "clock_time_logic.h"
#include "data.h"
#include "persona_logic.h"
#include "utf8_text_logic.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

// Advertise as "Codex-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Codex";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Codex-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"
const int W = 135, H = 240;
const int CX = W / 2;
const int CY_BASE = 120;
const int LED_PIN = 10;          // red LED, active-low

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background
// Keep the minimal validation surface available for future bring-up, but run
// the normal UI path by default now that approval rendering is fixed.
static const bool VALIDATION_UI = false;

const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
UsageMeterRenderState clockUsageMeterRenderState = {};
UsageMeterRenderState runtimeUsageMeterRenderState = {};
UsageMeterRenderState portraitUsageMeterRenderState = {};
SharedClockFaceCache standbyClockFaceCache = {};
SharedClockFaceCache runtimeClockFaceCache = {};
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { compatSetBrightnessPercent(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    compatSetDisplayEnabled(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

template <typename Canvas>
static void useDefaultTextFont(Canvas& canvas) {
  canvas.setFont(nullptr);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useUtf8FontForText(Canvas& canvas, const char* text, const lgfx::IFont* utf8Font) {
  if (text && utf8ContainsNonAscii(text)) {
    canvas.setFont(utf8Font);
    canvas.setTextSize(1);
  } else {
    canvas.setFont(nullptr);
    canvas.setTextSize(1);
  }
}

template <size_t N>
static void clipDisplayText(char (&out)[N], const char* text, uint8_t maxCells) {
  if (!text) {
    out[0] = 0;
    return;
  }
  size_t bytes = utf8ClipDisplayBytes(text, maxCells, N - 1);
  memcpy(out, text, bytes);
  out[bytes] = 0;
}

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static UsageMeterRenderFrame usageMeterFrameForDisplay(
  UsageMeterRenderState* renderState,
  uint16_t width,
  uint16_t height,
  bool forceDraw = false
) {
  UsageMeterState usage = {
    tama.hasUsageLimits,
    tama.fiveHourRemaining,
    tama.sevenDayRemaining,
  };
  return usageMeterPrepareFrame(renderState, tama.connected, usage, width, height, forceDraw);
}

template <typename Canvas>
static void paintUsageMeter(Canvas& canvas, const UsageMeterRenderPlan& plan) {
  for (uint8_t i = 0; i < plan.count; ++i) {
    const UsageMeterRect& rect = plan.rects[i];
    if (rect.width > 0) canvas.fillRect(rect.x, rect.y, rect.width, rect.height, rect.color);
  }
}

template <typename Canvas>
static void clearUsageMeter(Canvas& canvas, uint16_t width, uint16_t height, uint16_t color) {
  canvas.fillRect(0, height - USAGE_METER_FOOTPRINT, width, USAGE_METER_FOOTPRINT, color);
}

static uint8_t usageMeterBottomInset() {
  UsageMeterState usage = {
    tama.hasUsageLimits,
    tama.fiveHourRemaining,
    tama.sevenDayRemaining,
  };
  return usageMeterFooterInset(tama.connected, usage);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

static void invalidatePortraitSurface() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  usageMeterRenderReset(&portraitUsageMeterRenderState);
  characterInvalidate();
  buddyInvalidate();
}

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  invalidatePortraitSurface();
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const uint8_t SETTINGS_N = settingsMenuItemCount();

bool    wifiMenuOpen   = false;
bool    wifiStatusOpen = false;
uint8_t wifiMenuSel    = 0;
bool    otaReceiveScreen = false;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (settingsMenuAction(idx)) {
    case SETTINGS_BRIGHTNESS:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case SETTINGS_SOUND: s.sound = !s.sound; break;
    case SETTINGS_BLUETOOTH:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case SETTINGS_WIFI:
      settingsOpen = false;
      wifiMenuOpen = true;
      wifiStatusOpen = false;
      wifiMenuSel = 0;
      invalidatePortraitSurface();
      return;
    case SETTINGS_LED: s.led = !s.led; break;
    case SETTINGS_CLOCK_ROTATION: s.clockRot = (s.clockRot + 1) % 3; break;
    case SETTINGS_PET: nextPet(); return;
    case SETTINGS_OTA_UPDATE:
      if (!wifiManagerProvisioned()) {
        settingsOpen = false;
        wifiMenuOpen = true;
        wifiStatusOpen = false;
        wifiMenuSel = 0;
      } else {
        otaOfferOpenReceiveWindow(&tama.otaOffer, millis(), true);
        settingsOpen = false;
        otaReceiveScreen = true;
      }
      invalidatePortraitSurface();
      return;
    case SETTINGS_RESET:
      resetOpen = true;
      resetSel = 0;
      resetConfirmIdx = 0xFF;
      invalidatePortraitSurface();
      return;
    case SETTINGS_BACK:
      settingsOpen = false;
      invalidatePortraitSurface();
      return;
    case SETTINGS_INVALID: return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) {
    resetOpen = false;
    invalidatePortraitSurface();
    return;
  }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    wifiManagerForget();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

// Footer hint row inside a menu panel: "<downLbl> ↓  <rightLbl> →" with
// pixel triangles. Panels add MENU_HINT_H to height and call this at bottom.
const int MENU_HINT_H = 14;
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
  spr.drawFastHLine(mx + 6, hy - 4, mw - 12, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  // 6px/glyph at size 1; triangle goes 4px after the label ends
  int x = mx + 8;
  if (downLbl && downLbl[0]) {
    spr.setCursor(x, hy); spr.print(downLbl);
    x += strlen(downLbl) * 6 + 4;
    spr.fillTriangle(x, hy + 1, x + 6, hy + 1, x + 3, hy + 6, p.textDim);
  }
  if (rightLbl && rightLbl[0]) {
    x = mx + mw / 2 + 4;
    spr.setCursor(x, hy); spr.print(rightLbl);
    x += strlen(rightLbl) * 6 + 4;
    spr.fillTriangle(x, hy, x, hy + 6, x + 5, hy + 3, p.textDim);
  }
}

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + SETTINGS_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  Settings& s = settings();
  for (int i = 0; i < SETTINGS_N; i++) {
    SettingsMenuAction action = settingsMenuAction(i);
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsMenuLabel(i));
    spr.setCursor(mx + mw - 36, my + 8 + i * 14);
    spr.setTextColor(p.textDim, PANEL);
    if (action == SETTINGS_BRIGHTNESS) {
      spr.printf("%u/4", brightLevel);
    } else if (action == SETTINGS_SOUND
            || action == SETTINGS_BLUETOOTH
            || action == SETTINGS_WIFI
            || action == SETTINGS_LED) {
      bool value = action == SETTINGS_SOUND ? s.sound
          : action == SETTINGS_BLUETOOTH ? s.bt
          : action == SETTINGS_WIFI ? wifiManagerProvisioned()
          : s.led;
      spr.setTextColor(value ? GREEN : p.textDim, PANEL);
      spr.print(value ? " on" : "off");
    } else if (action == SETTINGS_CLOCK_ROTATION) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (action == SETTINGS_PET) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Change");
}

static void drawWifiMenu() {
  const Palette& p = characterPalette();
  bool provisioned = wifiManagerProvisioned();
  uint8_t count = wifiMenuItemCount(provisioned);
  int mw = 118, mh = 28 + count * 18 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.text, PANEL);
  spr.setCursor(mx + 7, my + 8);
  spr.print("WI-FI");
  for (uint8_t i = 0; i < count; ++i) {
    bool selected = i == wifiMenuSel;
    spr.setTextColor(selected ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 7, my + 26 + i * 18);
    spr.print(selected ? "> " : "  ");
    spr.print(wifiMenuLabel(provisioned, i));
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "Next", "Select");
}

static void drawWifiStatus() {
  const Palette& p = characterPalette();
  WifiManagerSnapshot wifi = wifiManagerSnapshot();
  int mw = 124, mh = 128;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4,
                    wifi.runtime.phase == WIFI_ERROR ? HOT : p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.text, PANEL);
  spr.setCursor(mx + 7, my + 8);
  spr.print("WI-FI");
  spr.setTextColor(p.textDim, PANEL);
  if (wifi.portalActive) {
    spr.setCursor(mx + 7, my + 25); spr.print("Join hotspot:");
    spr.setTextColor(p.body, PANEL);
    spr.setCursor(mx + 7, my + 38); spr.print(wifi.apSsid);
    spr.setTextColor(p.textDim, PANEL);
    spr.setCursor(mx + 7, my + 55); spr.print("Password:");
    spr.setTextColor(p.body, PANEL);
    spr.setCursor(mx + 7, my + 68); spr.print(wifi.apPassword);
    spr.setTextColor(p.textDim, PANEL);
    char clippedMessage[19]; wifiStatusClip(wifi.message, clippedMessage, sizeof(clippedMessage));
    spr.setCursor(mx + 7, my + 85);
    spr.print(wifi.runtime.phase == WIFI_CONNECTING ? "Connecting..." : clippedMessage);
  } else if (wifi.runtime.phase == WIFI_ONLINE) {
    char clippedSsid[19]; wifiStatusClip(wifi.ssid, clippedSsid, sizeof(clippedSsid));
    spr.setCursor(mx + 7, my + 27); spr.print("Connected");
    spr.setCursor(mx + 7, my + 44); spr.printf("SSID: %s", clippedSsid);
    spr.setCursor(mx + 7, my + 61); spr.printf("RSSI: %ld dBm", (long)wifi.rssi);
    spr.setCursor(mx + 7, my + 78); spr.printf("IP: %s", wifi.ip);
  } else if (wifi.runtime.provisioned) {
    char clippedSsid[19]; wifiStatusClip(wifi.ssid, clippedSsid, sizeof(clippedSsid));
    char clippedMessage[19]; wifiStatusClip(wifi.message, clippedMessage, sizeof(clippedMessage));
    spr.setCursor(mx + 7, my + 27); spr.print(clippedMessage);
    spr.setCursor(mx + 7, my + 44); spr.printf("SSID: %s", clippedSsid);
    spr.setCursor(mx + 7, my + 61); spr.print("RSSI: -");
    spr.setCursor(mx + 7, my + 78); spr.print("IP: -");
  } else {
    char clipped[19]; wifiStatusClip(wifi.message, clipped, sizeof(clipped));
    spr.setCursor(mx + 7, my + 31); spr.print(clipped[0] ? clipped : "Not configured");
  }
  drawMenuHints(p, mx, mw, my + mh - 12, "", wifi.portalActive ? "Cancel" : "Back");
}

static void drawOtaReceiveWindow() {
  const Palette& p = characterPalette();
  OtaUpdateView update = otaUpdateView();
  int mw = 124, mh = 124;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  spr.setTextColor(p.text, PANEL);
  spr.setCursor(mx + 7, my + 9);
  spr.print(update.visible ? "OTA UPDATE" : "OTA READY");
  spr.setTextColor(p.textDim, PANEL);
  if (update.visible) {
    spr.setCursor(mx + 7, my + 29);
    spr.print(update.status[0] ? update.status : "Preparing update");
    if (update.authenticated && update.version[0]) {
      spr.setCursor(mx + 7, my + 45);
      spr.printf("Version: %.12s", update.version);
    }
    if (update.phase >= OTA_PHASE_DOWNLOAD &&
        update.phase <= OTA_PHASE_FINAL_GATE) {
      int barW = mw - 14;
      spr.drawRect(mx + 7, my + 66, barW, 8, p.textDim);
      int fill = (barW - 2) * update.percent / 100;
      if (fill > 0) spr.fillRect(mx + 8, my + 67, fill, 6, p.body);
      spr.setCursor(mx + 7, my + 80);
      spr.printf("%u%%", update.percent);
    } else if (!update.authenticated) {
      spr.setCursor(mx + 7, my + 54);
      spr.print("Signed Mac update");
    }
    bool confirm = update.phase == OTA_PHASE_CONFIRM;
    drawMenuHints(p, mx, mw, my + mh - 12, confirm ? "Install" : "", "Cancel");
  } else {
    spr.setCursor(mx + 7, my + 30); spr.print("Run on your Mac:");
    spr.setTextColor(p.body, PANEL);
    spr.setCursor(mx + 7, my + 47); spr.print(otaUpdateCommandLine(0));
    spr.setCursor(mx + 7, my + 60); spr.print(otaUpdateCommandLine(1));
    spr.setTextColor(p.textDim, PANEL);
    uint32_t now = millis();
    uint32_t remaining = otaOfferWindowActive(tama.otaOffer, now)
      ? (tama.otaOffer.windowDeadlineMs - now + 999) / 1000 : 0;
    spr.setCursor(mx + 7, my + 78);
    spr.printf("Window: %lus", (unsigned long)remaining);
    spr.setCursor(mx + 7, my + 94);
    spr.print(tama.otaOffer.pending ? "Request received" : "Waiting for Mac");
    drawMenuHints(p, mx, mw, my + mh - 12, "", "Cancel");
  }
}

static void applyWifiMenuAction() {
  bool provisioned = wifiManagerProvisioned();
  switch (wifiMenuAction(provisioned, wifiMenuSel)) {
    case WIFI_MENU_STATUS:
      wifiStatusOpen = true;
      wifiMenuOpen = false;
      break;
    case WIFI_MENU_CHANGE:
    case WIFI_MENU_SETUP:
      wifiManagerStartProvisioning();
      wifiStatusOpen = true;
      wifiMenuOpen = false;
      break;
    case WIFI_MENU_FORGET:
      wifiManagerForget();
      wifiStatusOpen = true;
      wifiMenuOpen = false;
      break;
    case WIFI_MENU_BACK:
      wifiMenuOpen = false;
      settingsOpen = true;
      break;
    case WIFI_MENU_INVALID:
      return;
  }
  invalidatePortraitSurface();
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + RESET_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, HOT);
  spr.setTextSize(1);
  for (int i = 0; i < RESET_N; i++) {
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

void menuConfirm() {
  switch (menuSel) {
    case 0:
      settingsOpen = true;
      menuOpen = false;
      settingsSel = 0;
      invalidatePortraitSurface();
      break;
    case 1: compatPowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; invalidatePortraitSurface(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = 118, mh = 16 + MENU_N * 14 + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, 4, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, 4, p.textDim);
  spr.setTextSize(1);
  for (int i = 0; i < MENU_N; i++) {
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + 6, my + 8 + i * 14);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - 12);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. On the real StickS3 mount used here, that is the IMU Y axis,
// not X. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static int8_t  clockSwapFrames = 0;
static uint8_t runtimeOrient = 0;
static int8_t  runtimeOrientFrames = 0;
static int8_t  runtimeSwapFrames = 0;
static uint8_t paintedRuntimeOrient = 0;
static RuntimeLandscapeRenderState runtimeLandscapeRenderState = {};
static bool    runtimeLandscapePromptVisible = false;
static bool    previousStandbyClockFace = false;
static bool    previousLandscapeClockFace = false;
static bool    previousRuntimeSharedFace = false;
static bool    previousLandscapeRuntime = false;
static bool    previousPromptVisible = false;
static bool    _clockHostTimeValid = false;
static int64_t _clockHostLocalEpoch = 0;
static uint32_t _clockHostSyncMs = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and the shared clock face read this cache.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;

void clockOnTimeSync(int64_t local_epoch) {
  _clockHostTimeValid = true;
  _clockHostLocalEpoch = local_epoch;
  _clockHostSyncMs = millis();
}

static bool clockRefreshFromHostSync() {
  if (!_clockHostTimeValid) return false;
  int64_t local_epoch = _clockHostLocalEpoch + (int64_t)((millis() - _clockHostSyncMs) / 1000);
  ClockTimeFields fields = {};
  if (!clockFieldsFromLocalEpoch(local_epoch, &fields)) return false;
  _clkTm.Hours = fields.hours;
  _clkTm.Minutes = fields.minutes;
  _clkTm.Seconds = fields.seconds;
  _clkDt.year = fields.year;
  _clkDt.Month = fields.month;
  _clkDt.Date = fields.date;
  _clkDt.WeekDay = fields.week_day;
  return true;
}

static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = compatVbusVoltageMv() > 4000;
  if (clockRefreshFromHostSync()) return;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  clockOrientUpdateForStickS3(
    &clockOrient,
    &orientFrames,
    &clockSwapFrames,
    ax,
    ay,
    az,
    settings().clockRot
  );
}

static void runtimeUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  clockOrientUpdateForStickS3(
    &runtimeOrient,
    &runtimeOrientFrames,
    &runtimeSwapFrames,
    ax,
    ay,
    az,
    settings().clockRot
  );
}

static uint8_t clockDow() { return _clkDt.WeekDay; }

template <typename Canvas>
static void drawSharedClockFaceTo(
  Canvas& canvas,
  bool landscape,
  uint8_t orientation,
  SharedClockFaceCache* cache,
  UsageMeterRenderState* meterState,
  bool forceFullRepaint,
  bool promptExited
) {
  const Palette& p = characterPalette();
  const SharedClockFaceLayout layout = sharedClockFaceLayout(landscape);
  const bool fieldsValid = clockSharedFieldsValid(
    dataRtcValid() || _clockHostTimeValid,
    _clkTm.Hours,
    _clkTm.Minutes,
    _clkTm.Seconds,
    _clkDt.WeekDay,
    _clkDt.Month,
    _clkDt.Date
  );

  // Opening a new GIF state can clear the portrait sprite. Do it before the
  // cache decision so a persona transition and its text repaint are atomic.
  if (!buddyMode) {
    characterSetPeek(true);
    characterSetState(activeState);
  }

  UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
    meterState, layout.screenWidth, layout.screenHeight, false
  );
  SharedClockFaceRenderDecision decision = clockSharedFaceSchedule(
    cache,
    millis(),
    orientation,
    fieldsValid ? _clkTm.Seconds : -1,
    fieldsValid ? _clkDt.WeekDay : -1,
    fieldsValid ? _clkDt.Month : -1,
    fieldsValid ? _clkDt.Date : -1,
    activeState,
    forceFullRepaint,
    promptExited,
    meterFrame.decision.draw || meterFrame.decision.clear,
    false
  );

  if (decision.clearSurface) {
    canvas.fillScreen(p.bg);
    usageMeterRenderReset(meterState);
    meterFrame = usageMeterFrameForDisplay(
      meterState, layout.screenWidth, layout.screenHeight, true
    );
  } else if (meterFrame.decision.clear) {
    clearUsageMeter(canvas, layout.screenWidth, layout.screenHeight, p.bg);
  }

  // A functional screen may have selected a proportional/UTF-8 font before
  // this face. Restore the default fixed font before applying pixel geometry.
  useDefaultTextFont(canvas);
  if (decision.drawTime) {
    char hm[6];
    char seconds[4];
    clockFormatSharedTimeSegments(
      hm,
      sizeof(hm),
      seconds,
      sizeof(seconds),
      fieldsValid,
      _clkTm.Hours,
      _clkTm.Minutes,
      _clkTm.Seconds
    );
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(layout.time.textSize);
    canvas.setTextColor(p.text, p.bg);
    canvas.drawString(hm, layout.time.primary.x, layout.time.primary.y);
    canvas.setTextColor(p.textDim, p.bg);
    canvas.drawString(seconds, layout.time.seconds.x, layout.time.seconds.y);
  }

  if (decision.drawDate) {
    char dateLine[16];
    clockFormatSharedDateLine(
      dateLine,
      sizeof(dateLine),
      landscape,
      fieldsValid,
      _clkDt.WeekDay,
      _clkDt.Month,
      _clkDt.Date
    );
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(layout.date.textSize);
    canvas.setTextColor(p.textDim, p.bg);
    canvas.drawString(dateLine, layout.date.centerX, layout.date.centerY);
  }

  if (decision.drawPet) {
    if (buddyMode) {
      // ASCII species paint sparse glyphs and particles, so clear only the
      // shared compact pet rectangle. The time and usage regions stay intact.
      canvas.fillRect(layout.pet.x, layout.pet.y, layout.pet.width, layout.pet.height, p.bg);
      buddyRenderTo(
        &canvas,
        activeState,
        layout.pet.x + layout.pet.width / 2,
        layout.pet.y,
        1
      );
    } else {
      // The compact character renderer routes both GIF and text manifests.
      // Text frames clear only this pet rectangle; GIF frames self-erase so
      // they avoid a black flash before each decoded scanline.
      characterRenderCompactTo(
        &canvas,
        layout.pet.x + layout.pet.width / 2,
        layout.pet.y + layout.pet.height / 2,
        layout.pet.x,
        layout.pet.y,
        layout.pet.width,
        layout.pet.height,
        decision.fullRepaint
      );
    }
  }

  // Usage is always the final layer, so no pet/text refresh can cover it.
  if (meterFrame.decision.draw) paintUsageMeter(canvas, meterFrame.plan);
  canvas.setTextDatum(TL_DATUM);
  useDefaultTextFont(canvas);
}

static void drawSharedClockFace(
  bool landscape,
  uint8_t orientation,
  SharedClockFaceCache* cache,
  UsageMeterRenderState* meterState,
  bool forceFullRepaint = false,
  bool promptExited = false
) {
  if (landscape) {
    M5.Lcd.setRotation(orientation);
    drawSharedClockFaceTo(
      M5.Lcd,
      true,
      orientation,
      cache,
      meterState,
      forceFullRepaint,
      promptExited
    );
    M5.Lcd.setRotation(0);
    return;
  }
  drawSharedClockFaceTo(
    spr,
    false,
    0,
    cache,
    meterState,
    forceFullRepaint,
    promptExited
  );
}

PersonaState derive(const TamaState& s) {
  PersonaInputs input = {};
  input.connected = s.connected;
  input.sessionsRunning = s.sessionsRunning;
  input.sessionsWaiting = s.sessionsWaiting;
  return derivePersonaState(input);
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextColor(p.text, p.bg);
  useUtf8FontForText(spr, "Info", &fonts::efontCN_14);
  spr.setCursor(4, y); spr.print("Info");
  useDefaultTextFont(spr);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  useUtf8FontForText(spr, section, &fonts::efontCN_14);
  spr.setCursor(4, y); spr.print(section);
  y += utf8ContainsNonAscii(section) ? 14 : 12;
  useDefaultTextFont(spr);
}

void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(8, 56);  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(8, 184); spr.print("enter on desktop:");
  spr.setTextSize(3);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.setCursor((W - 18 * 6) / 2, 110);
  spr.print(b);
}

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[80]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    utf8TrimIncompleteTail(b);
    char line[80];
    clipDisplayText(line, b, 20);
    useUtf8FontForText(spr, line, &fonts::efontCN_10);
    spr.setCursor(4, y); spr.print(line);
    y += utf8ContainsNonAscii(line) ? 11 : 8;
    useDefaultTextFont(spr);
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Codex");
    ln("desktop sessions.");
    y += 6;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("Press A on a prompt");
    ln("to approve from here.");
    y += 6;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CODEX", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "open");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = compatBatteryVoltageMv();
    int iBat_mA = compatBatteryCurrentMa();
    int vBus_mV = compatVbusVoltageMv();
    int pct = (vBat_mV - 3200) / 10;   // (v-3.2)/(4.2-3.2)*100 = (v-3.2)*100 = (mv-3200)/10
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(1);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(60, y + 4);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += 8;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  ble      %s", dataBtActive() ? "linked" : "discover");
    ln("  temp     n/a");

  } else if (infoPage == 4) {
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = dataBtActive();

    spr.setTextColor(linked ? GREEN : HOT, p.bg);
    spr.setTextSize(2);
    spr.setCursor(4, y);
    spr.print(linked ? "linked" : "discover");
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 8;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" code-buddy");
      y += 4;
      ln("TO STAY LINKED");
      ln(" run codex");
    }

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    AboutInfo about = currentAboutInfo();
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("%s", about.made_by);
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 4;
    spr.setTextColor(p.text, p.bg);
    ln("%s", about.source_line_1);
    ln("%s", about.source_line_2);
    y += 12;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 4;
    ln("%s", about.hardware_line_1);
    ln("%s", about.hardware_line_2);
  }
}
static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = 84;
  const PortraitApprovalLayout layout = portraitApprovalLayout(
    usageMeterBottomInset() > 0
  );
  const int FOOTER_Y = layout.footerY;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  useDefaultTextFont(spr);
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(4, H - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  char toolLine[48];
  bool toolUtf8 = utf8ContainsNonAscii(tama.promptTool);
  uint16_t toolCells = utf8DisplayCells(tama.promptTool);
  clipDisplayText(toolLine, tama.promptTool, toolUtf8 ? 14 : (toolCells <= 10 ? 10 : 20));
  spr.setTextColor(p.text, p.bg);
  if (toolUtf8) {
    spr.setFont(&fonts::efontCN_14);
    spr.setTextSize(1);
    spr.setCursor(4, H - AREA + 18);
  } else {
    useDefaultTextFont(spr);
    spr.setTextSize(toolCells <= 10 ? 2 : 1);
    spr.setCursor(4, H - AREA + (toolCells <= 10 ? 14 : 18));
  }
  spr.print(toolLine);
  useDefaultTextFont(spr);

  // Hint wraps by display cells so multi-byte UTF-8 never gets split.
  char hintLines[8][48] = {};
  uint8_t hintRows = utf8WrapInto(tama.promptHint, hintLines, 8, 20, false);
  uint8_t hintBack = (hintRows > layout.maxHintRows)
      ? (hintRows - layout.maxHintRows)
      : 0;
  uint8_t hintOffset = utf8AutoScrollOffset(hintBack, millis() - promptArrivedMs);
  spr.setTextColor(p.textDim, p.bg);
  for (uint8_t i = 0; i < layout.maxHintRows && (hintOffset + i) < hintRows; ++i) {
    useUtf8FontForText(spr, hintLines[hintOffset + i], &fonts::efontCN_12);
    spr.setCursor(4, layout.hintStartY + i * layout.hintLineHeight);
    spr.print(hintLines[hintOffset + i]);
  }
  useDefaultTextFont(spr);

  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, FOOTER_Y);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, FOOTER_Y);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, FOOTER_Y);
    spr.print("B: deny");
  }
}

static void drawValidationScreen(bool inPrompt) {
  const Palette& p = characterPalette();
  UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
    &portraitUsageMeterRenderState, W, H, true
  );
  if (meterFrame.decision.clear) clearUsageMeter(spr, W, H, p.bg);
  spr.fillSprite(p.bg);
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, 8);
  spr.print("CodeBuddy BLE");
  spr.setTextColor((millis() / 500) % 2 ? GREEN : p.textDim, p.bg);
  spr.setCursor(W - 18, 8);
  spr.print("o");

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, 24);
  spr.printf("peer    %s", tama.connected ? "live" : "idle");
  spr.setCursor(6, 36);
  spr.printf("waiting %u", tama.sessionsWaiting);
  spr.setCursor(6, 48);
  spr.printf("prompt  %s", tama.promptId[0] ? "yes" : "no");
  spr.setCursor(6, 60);
  spr.print("msg ");
  char msgLine[32];
  clipDisplayText(msgLine, tama.msg, 16);
  useUtf8FontForText(spr, msgLine, &fonts::efontCN_12);
  spr.print(msgLine);
  useDefaultTextFont(spr);

  if (inPrompt) {
    drawApproval();
  } else {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(6, 92);
    spr.print("waiting for prompt");
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(6, 106);
    spr.print("host link only mode");
  }

  if (meterFrame.decision.draw) paintUsageMeter(spr, meterFrame.plan);
  spr.pushSprite(0, 0);
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 2, y, 2, col);
    spr.fillCircle(x + 2, y, 2, col);
    spr.fillTriangle(x - 4, y + 1, x + 4, y + 1, x, y + 5, col);
  } else {
    spr.drawCircle(x - 2, y, 2, col);
    spr.drawCircle(x + 2, y, 2, col);
    spr.drawLine(x - 4, y + 1, x, y + 5, col);
    spr.drawLine(x + 4, y + 1, x, y + 5, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 16;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(54 + i * 16, y + 2, i < mood, moodCol);

  y += 20;
  spr.setCursor(6, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = 38 + i * 9;
    if (i < fed) spr.fillCircle(px, y + 1, 2, p.body);
    else spr.drawCircle(px, y + 1, 2, p.textDim);
  }

  y += 20;
  spr.setCursor(6, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = 54 + i * 13;
    if (i < en) spr.fillRect(px, y - 2, 9, 6, enCol);
    else spr.drawRect(px, y - 2, 9, 6, p.textDim);
  }

  y += 24;
  spr.fillRoundRect(6, y - 2, 42, 14, 3, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(11, y + 1); spr.printf("Lv %u", stats().level);

  y += 20;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(6, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(6, y + 10);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(6, y + 20);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(6, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + 30);
  tokFmt("today    ", tama.tokensToday, y + 40);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = 70;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(1);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  y += 12;  // room for the PET header drawn by drawPet()

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
  ln(p.textDim, "any button = wake"); gap();

  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = 70;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  // Header on top of whichever page drew — title left, counter right
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(4, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

static uint32_t runtimeLandscapeHashText(uint32_t hash, const char* text) {
  if (!text) return hash ^ 0xFFU;
  for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
    hash = (hash ^ *p) * 16777619UL;
  }
  return (hash ^ 0xFFU) * 16777619UL;
}

static uint32_t runtimePromptRevision() {
  uint32_t hash = 2166136261UL;
  hash = runtimeLandscapeHashText(hash, tama.promptId);
  hash = runtimeLandscapeHashText(hash, tama.promptTool);
  hash = runtimeLandscapeHashText(hash, tama.promptHint);
  return responseSent ? (hash ^ 0xA5A5A5A5UL) : hash;
}

static uint8_t runtimePromptScrollOffset(uint32_t now) {
  char hintLines[8][48] = {};
  uint8_t hintRows = utf8WrapInto(tama.promptHint, hintLines, 8, 36, false);
  uint8_t hintBack = (hintRows > 2) ? (hintRows - 2) : 0;
  return utf8AutoScrollOffset(hintBack, now - promptArrivedMs);
}

static void drawLandscapeApproval(const Palette& p, uint8_t hintOffset) {
  const int LW = 240, LH = 135, AREA = 88;
  const int FOOTER_Y = LH - 12 - usageMeterBottomInset();
  M5.Lcd.fillRect(0, LH - AREA, LW, AREA, p.bg);
  M5.Lcd.drawFastHLine(0, LH - AREA, LW, p.textDim);

  useDefaultTextFont(M5.Lcd);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(p.textDim, p.bg);
  M5.Lcd.setCursor(4, LH - AREA + 4);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) M5.Lcd.setTextColor(HOT, p.bg);
  M5.Lcd.printf("approve? %lus", (unsigned long)waited);

  char toolLine[48];
  clipDisplayText(toolLine, tama.promptTool, utf8ContainsNonAscii(tama.promptTool) ? 24 : 32);
  useUtf8FontForText(M5.Lcd, toolLine, &fonts::efontCN_14);
  M5.Lcd.setTextColor(p.text, p.bg);
  M5.Lcd.setCursor(4, LH - AREA + 18);
  M5.Lcd.print(toolLine);
  useDefaultTextFont(M5.Lcd);

  char hintLines[8][48] = {};
  uint8_t hintRows = utf8WrapInto(tama.promptHint, hintLines, 8, 36, false);
  M5.Lcd.setTextColor(p.textDim, p.bg);
  for (uint8_t i = 0; i < 2 && (hintOffset + i) < hintRows; ++i) {
    useUtf8FontForText(M5.Lcd, hintLines[hintOffset + i], &fonts::efontCN_12);
    M5.Lcd.setCursor(4, LH - AREA + 34 + i * 12);
    M5.Lcd.print(hintLines[hintOffset + i]);
  }
  useDefaultTextFont(M5.Lcd);

  if (responseSent) {
    M5.Lcd.setTextColor(p.textDim, p.bg);
    M5.Lcd.setCursor(4, FOOTER_Y);
    M5.Lcd.print("sent...");
  } else {
    M5.Lcd.setTextColor(GREEN, p.bg);
    M5.Lcd.setCursor(4, FOOTER_Y);
    M5.Lcd.print("A: approve");
    M5.Lcd.setTextColor(HOT, p.bg);
    M5.Lcd.setCursor(LW - 52, FOOTER_Y);
    M5.Lcd.print("B: deny");
  }
}

static void drawRuntimeLandscape(bool inPrompt) {
  const Palette& p = characterPalette();
  const RuntimePetLayout layout = runtimePetLayout(true);
  uint32_t now = millis();
  M5.Lcd.setRotation(runtimeOrient);
  bool promptExited = runtimeNeedsFullRepaintOnPromptExit(
    runtimeLandscapePromptVisible,
    inPrompt
  );
  bool promptEntered = !runtimeLandscapePromptVisible && inPrompt;
  bool repaint = paintedRuntimeOrient != runtimeOrient || promptEntered || promptExited;
  bool overlayVisible = runtimeStatusOverlayVisible(inPrompt);
  uint32_t overlayContentRevision = 0;
  uint32_t overlayTimeRevision = 0;
  uint8_t overlayScrollOffset = 0;
  if (inPrompt) {
    overlayContentRevision = runtimePromptRevision();
    overlayTimeRevision = (now - promptArrivedMs) / 1000;
    overlayScrollOffset = runtimePromptScrollOffset(now);
  }
  RuntimeLandscapeRenderDecision decision = runtimeLandscapeSchedule(
    &runtimeLandscapeRenderState,
    now,
    repaint,
    overlayVisible,
    overlayContentRevision,
    overlayTimeRevision,
    overlayScrollOffset
  );
  if (decision.repaint) {
    M5.Lcd.fillScreen(p.bg);
    paintedRuntimeOrient = runtimeOrient;
    usageMeterRenderReset(&runtimeUsageMeterRenderState);
    if (promptExited && !buddyMode) characterInvalidate();
  }
  UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
    &runtimeUsageMeterRenderState, 240, 135, decision.repaint || decision.overlay
  );
  if (meterFrame.decision.clear) {
    clearUsageMeter(M5.Lcd, 240, 135, p.bg);
  }

  bool renderPet = decision.pet && (!inPrompt || decision.repaint);
  if (renderPet && buddyMode) {
    if (inPrompt) {
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      // Clear the whole pet viewport: some species animate particles above
      // their centered body. The viewport ends where the meter footprint
      // begins, and the meters are repainted last below.
      RuntimePetClearRect clear = runtimePetClearRect(true);
      M5.Lcd.fillRect(clear.x, clear.y, clear.width, clear.height, p.bg);
      buddyRenderTo(
        &M5.Lcd,
        activeState,
        layout.centerX,
        layout.asciiYOffset,
        layout.asciiScale
      );
    }
  } else if (renderPet) {
    characterSetState(activeState);
    if (inPrompt) {
      characterRenderTo(&M5.Lcd, 57, 45);
    } else {
      characterRenderRuntimeTo(
        &M5.Lcd,
        layout.centerX,
        layout.centerY,
        layout.viewportWidth,
        layout.viewportHeight,
        decision.repaint
      );
    }
  }

  if (decision.overlay && inPrompt) drawLandscapeApproval(p, overlayScrollOffset);
  runtimeLandscapePromptVisible = inPrompt;
  if (meterFrame.decision.draw) paintUsageMeter(M5.Lcd, meterFrame.plan);
  M5.Lcd.setRotation(0);
}

void setup() {
  auto cfg = M5.config();
  cfg.output_power = true;
  cfg.internal_imu = true;
  cfg.internal_spk = true;
  cfg.internal_rtc = true;
  M5.begin(cfg);
  Serial.begin(115200);
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 500) delay(10);
  Serial.println("[boot] setup");
  M5.Lcd.setRotation(0);
  M5.Imu.Init();
  M5.Beep.begin();
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);   // off
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  wifiManagerBegin(strlen(btName) > 6 ? btName + 6 : "SETUP");
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.createSprite(W, H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
      &portraitUsageMeterRenderState, W, H, true
    );
    if (meterFrame.decision.clear) clearUsageMeter(spr, W, H, p.bg);
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(2);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 12);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 12);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 12);
      spr.setTextSize(1);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 12);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(1);
    if (meterFrame.decision.draw) paintUsageMeter(spr, meterFrame.plan);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (activeState == P_ATTENTION && settings().led) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      Serial.printf(
        "[prompt] show id=%s tool=%s mode=%u\n",
        tama.promptId, tama.promptTool, displayMode
      );
      promptArrivedMs = millis();
      napping = false;
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      otaReceiveScreen = false;
      otaUpdateCancel();
      otaOfferCancel(&tama.otaOffer);
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;
  wifiManagerPoll(inPrompt);
  int batteryVoltageMv = compatBatteryVoltageMv();
  int batteryPercent = (batteryVoltageMv - 3200) / 10;
  if (batteryPercent < 0) batteryPercent = 0;
  if (batteryPercent > 100) batteryPercent = 100;
  OtaUpdateRuntimeInputs otaInputs = {};
  otaInputs.bleConnected = bleConnected();
  otaInputs.wifiProvisioned = wifiManagerProvisioned();
  otaInputs.wifiOnline = wifiManagerOnline();
  otaInputs.trustedTime = wifiManagerSystemTimeTrusted();
  otaInputs.prompt = inPrompt;
  otaInputs.transfer = xferActive();
  otaInputs.provisioning = wifiManagerUiActive();
  otaInputs.passkey = blePasskey() != 0;
  otaInputs.functional = menuOpen || settingsOpen || wifiMenuOpen ||
    wifiStatusOpen || resetOpen || displayMode != DISP_NORMAL;
  otaInputs.externalPower = compatVbusVoltageMv() > 4000;
  otaInputs.batteryKnown = batteryVoltageMv >= 2500 && batteryVoltageMv <= 5000;
  otaInputs.batteryPercent = static_cast<uint8_t>(batteryPercent);
  otaUpdatePoll(&tama.otaOffer, otaInputs);
  if (otaUpdateActive()) {
    napping = false;
    wake();
  }
  if (otaReceiveScreen && !otaUpdateActive() &&
      !otaOfferWindowActive(tama.otaOffer, now)) {
    otaReceiveScreen = false;
    invalidatePortraitSurface();
  }

  if (VALIDATION_UI) {
    napping = false;
    if (screenOff) {
      compatSetDisplayEnabled(true);
      screenOff = false;
    }

    if (M5.BtnA.wasReleased() && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
      Serial.printf("[prompt] approve id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      uint32_t tookS = (millis() - promptArrivedMs) / 1000;
      statsOnApproval(tookS);
      beep(2400, 60);
      if (tookS < 5) triggerOneShot(P_HEART, 2000);
      inPrompt = false;
    }

    if (M5.BtnB.wasReleased() && inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      Serial.printf("[prompt] deny id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
      inPrompt = false;
    }

    drawValidationScreen(inPrompt);
    delay(16);
    return;
  }

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (compatPowerKeyState() == 0x02) {
    if (otaUpdateActive()) {
      wake();
    } else if (screenOff) {
      wake();
    } else {
      compatSetDisplayEnabled(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (otaReceiveScreen) {
      if (otaUpdateActive()) {
        otaUpdateCancel();
      } else {
        otaOfferCancel(&tama.otaOffer);
        otaReceiveScreen = false;
        settingsOpen = true;
        invalidatePortraitSurface();
      }
    }
    else if (wifiStatusOpen) {
      if (wifiManagerUiActive()) wifiManagerCancelProvisioning();
      wifiStatusOpen = false;
      wifiMenuOpen = true;
      wifiMenuSel = 0;
      invalidatePortraitSurface();
    }
    else if (wifiMenuOpen) {
      wifiMenuOpen = false;
      settingsOpen = true;
      invalidatePortraitSurface();
    }
    else if (resetOpen) { resetOpen = false; invalidatePortraitSurface(); }
    else if (settingsOpen) { settingsOpen = false; invalidatePortraitSurface(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) invalidatePortraitSurface();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        Serial.printf("[prompt] approve id=%s\n", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (otaReceiveScreen) {
        if (otaUpdateActive() && otaUpdateView().phase == OTA_PHASE_CONFIRM) {
          beep(1800, 40);
          otaUpdateConfirm();
        }
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (wifiMenuOpen) {
        beep(1800, 30);
        uint8_t count = wifiMenuItemCount(wifiManagerProvisioned());
        wifiMenuSel = (wifiMenuSel + 1) % count;
      } else if (wifiStatusOpen) {
        // Status/provisioning is an informational surface. B returns/cancels.
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      Serial.printf("[prompt] deny id=%s\n", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (otaReceiveScreen) {
      beep(600, 40);
      if (otaUpdateActive()) {
        otaUpdateCancel();
      } else {
        otaOfferCancel(&tama.otaOffer);
        otaReceiveScreen = false;
        settingsOpen = true;
        invalidatePortraitSurface();
      }
    } else if (wifiStatusOpen) {
      beep(2400, 30);
      if (wifiManagerUiActive()) wifiManagerCancelProvisioning();
      wifiStatusOpen = false;
      wifiMenuOpen = true;
      wifiMenuSel = 0;
      invalidatePortraitSurface();
    } else if (wifiMenuOpen) {
      beep(2400, 30);
      applyWifiMenuAction();
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    }
  }

  // blink bookkeeping

  // Refresh once per second for both standby and active shared clock faces.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  SharedClockFaceContext sharedContext = {};
  sharedContext.normalDisplay = displayMode == DISP_NORMAL;
  sharedContext.menuVisible = menuOpen;
  sharedContext.settingsVisible = settingsOpen || wifiMenuOpen || wifiStatusOpen
    || otaReceiveScreen;
  sharedContext.resetVisible = resetOpen;
  sharedContext.passkeyVisible = blePasskey() != 0;
  sharedContext.promptVisible = inPrompt;
  sharedContext.functionalOverrideVisible = xferActive() || wifiManagerUiActive()
    || otaReceiveScreen;
  sharedContext.otaProgressVisible = otaUpdateActive();
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb
               && sharedClockFaceSelected(sharedContext, SHARED_CLOCK_IDLE);
  SharedClockActivity sharedActivity = tama.sessionsRunning > 0
    ? SHARED_CLOCK_ACTIVE
    : tama.sessionsWaiting > 0 ? SHARED_CLOCK_WAITING : SHARED_CLOCK_IDLE;
  bool runtimeSharedFace = (tama.sessionsRunning > 0 || tama.sessionsWaiting > 0)
    && sharedClockFaceSelected(sharedContext, sharedActivity);
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; clockSwapFrames = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  // Codex activity gets the same StickS3 auto-orientation policy as the
  // charging clock, but only on the normal home surface. The portrait sprite
  // remains unrotated; a landscape runtime uses the direct LCD path below.
  bool runtimeOrienting = screenOrientRuntimeEligible(
    displayMode == DISP_NORMAL,
    menuOpen,
    settingsOpen || wifiMenuOpen || wifiStatusOpen || otaReceiveScreen,
    resetOpen,
    runtimeSharedFace,
    false,
    inPrompt
  );
  if (runtimeOrienting) runtimeUpdateOrient();
  else {
    runtimeOrient = 0;
    runtimeOrientFrames = 0;
    runtimeSwapFrames = 0;
    paintedRuntimeOrient = 0;
  }
  bool landscapeRuntime = runtimeOrienting && runtimeOrient != 0;
  bool portraitSharedFace = (clocking && !landscapeClock)
    || (runtimeSharedFace && !landscapeRuntime);

  if (clocking != previousStandbyClockFace ||
      landscapeClock != previousLandscapeClockFace) {
    if (!landscapeClock || landscapeClock != previousLandscapeClockFace) {
      usageMeterRenderReset(&clockUsageMeterRenderState);
      clockSharedFaceCacheReset(&standbyClockFaceCache);
    }
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    previousStandbyClockFace = clocking;
    previousLandscapeClockFace = landscapeClock;
  }

  if (runtimeSharedFace != previousRuntimeSharedFace) {
    clockSharedFaceCacheReset(&runtimeClockFaceCache);
    usageMeterRenderReset(&runtimeUsageMeterRenderState);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    if (!runtimeSharedFace) applyDisplayMode();
    previousRuntimeSharedFace = runtimeSharedFace;
  }

  if (landscapeRuntime != previousLandscapeRuntime) {
    // Direct LCD rendering must leave the display in its native portrait
    // rotation before the sprite path resumes.
    M5.Lcd.setRotation(0);
    usageMeterRenderReset(&runtimeUsageMeterRenderState);
    clockSharedFaceCacheReset(&runtimeClockFaceCache);
    if (landscapeRuntime) {
      characterSetPeek(true);
      buddySetPeek(true);
    } else {
      applyDisplayMode();
      runtimeLandscapeRenderState = {};
      runtimeLandscapePromptVisible = false;
    }
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    previousLandscapeRuntime = landscapeRuntime;
  }

  bool promptExited = runtimeNeedsFullRepaintOnPromptExit(previousPromptVisible, inPrompt);
  if (promptExited) {
    // The approval scheduler owns separate overlay state. Reset it when the
    // shared face resumes so a later prompt entry always starts cleanly.
    runtimeLandscapeRenderState = {};
    runtimeLandscapePromptVisible = false;
    paintedRuntimeOrient = 0;
  }
  characterSetRuntimeViewport(false);
  if (promptExited && !landscapeRuntime) {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    usageMeterRenderReset(&portraitUsageMeterRenderState);
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (napping || screenOff || landscapeClock || landscapeRuntime || portraitSharedFace) {
    // skip sprite render — face-down, powered off, or a direct-to-LCD
    // landscape surface below.
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeRuntime && !napping && !screenOff) {
    if (inPrompt) drawRuntimeLandscape(true);
    else if (runtimeSharedFace) {
      drawSharedClockFace(
        true,
        runtimeOrient,
        &runtimeClockFaceCache,
        &runtimeUsageMeterRenderState,
        false,
        promptExited
      );
    }
  } else if (inPrompt) {
    const Palette& p = characterPalette();
    UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
      &portraitUsageMeterRenderState, W, H, true
    );
    if (meterFrame.decision.clear) clearUsageMeter(spr, W, H, p.bg);
    drawApproval();
    if (meterFrame.decision.draw) paintUsageMeter(spr, meterFrame.plan);
    spr.pushSprite(0, 0);
  } else if (landscapeClock) {
    drawSharedClockFace(
      true,
      clockOrient,
      &standbyClockFaceCache,
      &clockUsageMeterRenderState
    );
  } else if (!napping && !screenOff) {
    if (clocking) {
      drawSharedClockFace(
        false,
        0,
        &standbyClockFaceCache,
        &clockUsageMeterRenderState
      );
      spr.pushSprite(0, 0);
    } else if (runtimeSharedFace) {
      drawSharedClockFace(
        false,
        0,
        &runtimeClockFaceCache,
        &runtimeUsageMeterRenderState,
        false,
        promptExited
      );
      spr.pushSprite(0, 0);
    } else {
      const Palette& p = characterPalette();
      UsageMeterRenderFrame meterFrame = usageMeterFrameForDisplay(
        &portraitUsageMeterRenderState, W, H, true
      );
      if (meterFrame.decision.clear) clearUsageMeter(spr, W, H, p.bg);
      if (blePasskey()) drawPasskey();
      else if (displayMode == DISP_INFO) drawInfo();
      else if (displayMode == DISP_PET) drawPet();
      if (otaReceiveScreen) drawOtaReceiveWindow();
      else if (wifiStatusOpen) drawWifiStatus();
      else if (wifiMenuOpen) drawWifiMenu();
      else if (resetOpen) drawReset();
      else if (settingsOpen) drawSettings();
      else if (menuOpen) drawMenu();
      if (meterFrame.decision.draw) paintUsageMeter(spr, meterFrame.plan);
      spr.pushSprite(0, 0);
    }
  }
  previousPromptVisible = inPrompt;

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt && !otaUpdateActive()) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    compatSetBrightnessPercent(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && !wifiManagerUiActive()
      && !otaUpdateActive()
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    compatSetDisplayEnabled(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
