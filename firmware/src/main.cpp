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
#include "ota_boot_health.h"
#include "ota_update.h"
#include "ota_ui_logic.h"
#include "clock_time_logic.h"
#include "fonts/jetbrains_mono_ascii_8.h"
#include "landscape_dashboard_logic.h"
#include "completion_chime_logic.h"
#include "data.h"
#include "persona_logic.h"
#include "utf8_text_logic.h"
#include "buddy.h"

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
TFT_eSprite landscapeClockPetSprite = TFT_eSprite(&M5.Lcd);
TFT_eSprite landscapeHeartbeatSprite = TFT_eSprite(&M5.Lcd);
static bool landscapeClockPetSpriteReady = false;
static bool landscapeHeartbeatSpriteReady = false;

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
static CompletionChimeState completionChimeState = {};
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
static void useSharedFaceAsciiFont(Canvas& canvas) {
  canvas.setFont(&code_buddy_fonts::JetBrainsMono_Regular8pt7b);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useDashboardStatusFont(Canvas& canvas) {
  canvas.setFont(&code_buddy_fonts::JetBrainsMono_Regular7pt7b);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useDashboardTimeFont(Canvas& canvas) {
  canvas.setFont(&code_buddy_fonts::JetBrainsMono_Regular20pt7b);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useDashboardSecondsFont(Canvas& canvas) {
  canvas.setFont(&code_buddy_fonts::JetBrainsMono_Regular14pt7b);
  canvas.setTextSize(1);
}

template <typename Canvas>
static void useDashboardCardFont(Canvas& canvas) {
  canvas.setFont(&code_buddy_fonts::JetBrainsMono_Bold6pt7b);
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

static void playCompletionSound() {
  if (!settings().sound) return;
  static const int COMPLETION_SOUND_CHANNEL = 0;
  M5.Speaker.tone(1600, 70, COMPLETION_SOUND_CHANNEL, true);
  M5.Speaker.tone(2400, 100, COMPLETION_SOUND_CHANNEL, false);
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
    tama.hasFiveHourUsage,
    tama.hasSevenDayUsage,
    tama.fiveHourRemaining,
    tama.sevenDayRemaining,
  };
  return usageMeterPrepareFrame(renderState, tama.connected, usage, width, height, forceDraw);
}

static UsageMeterRenderFrame usageMeterLandscapeFrameForDisplay(
  UsageMeterRenderState* renderState,
  uint16_t width,
  uint16_t height,
  bool forceDraw,
  bool animationActive,
  uint8_t animationFrame
) {
  UsageMeterState usage = {
    tama.hasFiveHourUsage,
    tama.hasSevenDayUsage,
    tama.fiveHourRemaining,
    tama.sevenDayRemaining,
  };
  return usageMeterPrepareLandscapeSingleFrame(
    renderState,
    tama.connected,
    usage,
    width,
    height,
    forceDraw,
    animationActive,
    animationFrame
  );
}

static uint16_t statusDashboardColor(
  StatusDashboardColorRole role,
  const Palette& palette
) {
  switch (role) {
    case STATUS_COLOR_GREEN: return 0x07E0;
    case STATUS_COLOR_AMBER: return 0xFD20;
    case STATUS_COLOR_CYAN: return 0x07FF;
    case STATUS_COLOR_DIM:
    default: return palette.textDim;
  }
}

template <typename Canvas>
static void paintUsageMeter(
  Canvas& canvas,
  const UsageMeterRenderPlan& plan,
  bool animationActive = false,
  uint8_t animationFrame = 0
) {
  if (plan.dotted) {
    for (uint8_t row = 0; row < plan.dotRows; ++row) {
      for (uint16_t column = 0; column < plan.dotColumns; ++column) {
        const UsageMeterRect dot = usageMeterDotRect(plan, column, row);
        canvas.fillRect(
          dot.x,
          dot.y,
          dot.width,
          dot.height,
          usageMeterDotColor(plan, column, row, animationActive, animationFrame)
        );
      }
    }
    return;
  }
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
    tama.hasFiveHourUsage,
    tama.hasSevenDayUsage,
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
bool    otaCompactOverlay = false;

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
    case SETTINGS_AUTO_OTA: s.autoOta = !s.autoOta; break;
    case SETTINGS_OTA_UPDATE:
      if (!wifiManagerProvisioned()) {
        settingsOpen = false;
        wifiMenuOpen = true;
        wifiStatusOpen = false;
        wifiMenuSel = 0;
      } else {
        otaOfferOpenReceiveWindow(&tama.otaOffer, millis(), true);
        settingsOpen = false;
        otaCompactOverlay = false;
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
    } else if (action == SETTINGS_AUTO_OTA) {
      spr.setTextColor(s.autoOta ? GREEN : p.textDim, PANEL);
      spr.print(s.autoOta ? " on" : "off");
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
    OtaUiPlan plan = otaUiPlan(
      update.visible, update.phase == OTA_PHASE_CONFIRM,
      update.automatic, update.bootCommitted, update.cancellable
    );
    spr.setCursor(mx + 7, my + 29);
    spr.print(update.status[0] ? update.status : "Preparing update");
    if (update.version[0]) {
      spr.setCursor(mx + 7, my + 45);
      spr.printf("Version: %.12s", update.version);
    }
    if (update.sizeBytes) {
      char readableSize[16] = {};
      otaFormatReadableSize(update.sizeBytes, readableSize, sizeof(readableSize));
      spr.setCursor(mx + 7, my + 57);
      spr.printf("Size: %s", readableSize);
    }
    if (update.phase >= OTA_PHASE_DOWNLOAD &&
        update.phase <= OTA_PHASE_FINAL_GATE) {
      int barW = mw - 14;
      spr.drawRect(mx + 7, my + 72, barW, 8, p.textDim);
      int fill = (barW - 2) * update.percent / 100;
      if (fill > 0) spr.fillRect(mx + 8, my + 73, fill, 6, p.body);
      spr.setCursor(mx + 7, my + 84);
      spr.printf("%u%%", update.percent);
    } else if (!update.authenticated) {
      spr.setCursor(mx + 7, my + 72);
      spr.print("Signed Mac update");
    }
    drawMenuHints(
      p, mx, mw, my + mh - 12,
      plan.showInstall ? "Install" : "", plan.showCancel ? "Cancel" : ""
    );
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

template <typename Canvas>
static void drawOtaCompactOverlayTo(
  Canvas& canvas,
  bool landscape,
  const OtaUpdateView& update
) {
  const Palette& p = characterPalette();
  const OtaCompactOverlayLayout layout = otaCompactOverlayLayout(landscape);
  canvas.fillRoundRect(layout.x, layout.y, layout.width, layout.height, 4, PANEL);
  canvas.drawRoundRect(layout.x, layout.y, layout.width, layout.height, 4, p.textDim);
  useDefaultTextFont(canvas);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(p.text, PANEL);
  canvas.setCursor(layout.x + 7, layout.y + 5);
  if (update.version[0]) canvas.printf("OTA %.12s", update.version);
  else canvas.print("OTA");

  char percent[6] = {};
  snprintf(percent, sizeof(percent), "%u%%", update.percent);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(percent, layout.x + layout.width - 7, layout.y + 5);

  char status[25] = {};
  clipDisplayText(
    status,
    update.status[0] ? update.status : "Preparing update",
    landscape ? 24 : 17
  );
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(p.textDim, PANEL);
  canvas.drawString(status, layout.x + 7, layout.y + 18);

  const int16_t barX = layout.x + 7;
  const int16_t barY = layout.y + layout.height - 10;
  const int16_t barW = layout.width - 14;
  canvas.drawRect(barX, barY, barW, 6, p.textDim);
  const int16_t fill = (barW - 2) * update.percent / 100;
  if (fill > 0) canvas.fillRect(barX + 1, barY + 1, fill, 4, p.body);
  canvas.setTextDatum(TL_DATUM);
  useDefaultTextFont(canvas);
}

static void drawLandscapeOtaCompactOverlay(
  uint8_t orientation,
  const OtaUpdateView& update
) {
  M5.Lcd.setRotation(orientation);
  drawOtaCompactOverlayTo(M5.Lcd, true, update);
  M5.Lcd.setRotation(0);
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
static ClockOrientationState clockOrientationState = {};
static ClockOrientationState runtimeOrientationState = {};
static ScreenOrientationRenderState clockAutoSurfaceRenderState = {};
static ScreenOrientationRenderState runtimeAutoSurfaceRenderState = {};
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
  clockOrientUpdateStateForStickS3(
    &clockOrientationState,
    ax,
    ay,
    az,
    settings().clockRot
  );
}

static void runtimeUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  clockOrientUpdateStateForStickS3(
    &runtimeOrientationState,
    ax,
    ay,
    az,
    settings().clockRot
  );
}

static uint8_t clockDow() { return _clkDt.WeekDay; }

template <typename Canvas>
static void drawLandscapeDashboardStatus(Canvas& canvas) {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  const LandscapeDashboardStatus status = landscapeDashboardStatus(
    tama.connected,
    tama.sessionsRunning,
    tama.sessionsWaiting
  );
  const uint16_t color = landscapeDashboardStatusColor(status);
  canvas.fillRect(0, 0, 120, 18, LANDSCAPE_DASHBOARD_BG);
  canvas.fillRoundRect(
    layout.statusDotX,
    layout.statusDotY,
    layout.statusDotSize,
    layout.statusDotSize,
    layout.statusDotRadius,
    color
  );
  useDashboardStatusFont(canvas);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(color, LANDSCAPE_DASHBOARD_BG);
  canvas.drawString(
    landscapeDashboardStatusLabel(status),
    layout.statusLabelX,
    layout.statusY
  );
}

template <typename Canvas>
static void drawLandscapeDashboardHeartbeat(
  Canvas& canvas,
  uint32_t now,
  bool force
) {
  static_assert(
    TOKEN_HEARTBEAT_SAMPLE_COUNT == LANDSCAPE_DASHBOARD_TOKEN_SAMPLE_COUNT,
    "token heartbeat payload must map one sample to each graph pixel"
  );
  if (!landscapeDashboardHeartbeatCanPresent(landscapeHeartbeatSpriteReady)) {
    // Retain the last complete frame rather than tearing the LCD with a
    // clear-then-redraw fallback when the off-screen buffer is unavailable.
    return;
  }
  static uint8_t lastFrameToken = UINT8_MAX;
  static uint32_t lastActivityReceipt = UINT32_MAX;
  static uint32_t lastTokenReceipt = UINT32_MAX;
  static LandscapeDashboardHeartbeatSource lastSource =
    LANDSCAPE_HEARTBEAT_NONE;
  const LandscapeDashboardHeartbeatSource source =
    landscapeDashboardHeartbeatSource(
      tama.tokenHeartbeat.valid,
      tama.hasActivity20
    );
  const bool activityChanged =
    tama.activity20ReceivedAt != lastActivityReceipt;
  const bool tokenChanged =
    tama.tokenHeartbeat.receivedAtMs != lastTokenReceipt;
  const uint8_t frameToken = source == LANDSCAPE_HEARTBEAT_TOKEN
    ? landscapeDashboardTokenHeartbeatFrameToken(
        tama.tokenHeartbeat.receivedAtMs,
        now
      )
    : (source == LANDSCAPE_HEARTBEAT_ACTIVITY
      ? landscapeDashboardHeartbeatFrameToken(tama.activity20ReceivedAt, now)
      : 0);
  if (!force && source == lastSource && !activityChanged && !tokenChanged &&
      frameToken == lastFrameToken) {
    return;
  }
  lastFrameToken = frameToken;
  lastActivityReceipt = tama.activity20ReceivedAt;
  lastTokenReceipt = tama.tokenHeartbeat.receivedAtMs;
  lastSource = source;

  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  const uint32_t activityMask = source == LANDSCAPE_HEARTBEAT_ACTIVITY
    ? landscapeDashboardActivityMaskAt(
        tama.activity20,
        tama.activity20ReceivedAt,
        now
      )
    : 0;
  const uint32_t latestBucket = source == LANDSCAPE_HEARTBEAT_ACTIVITY
    ? landscapeDashboardHeartbeatLatestBucket(
        tama.activity20ReceivedAt, now
      )
    : 0;
  const uint8_t scrollPixels = source == LANDSCAPE_HEARTBEAT_ACTIVITY
    ? landscapeDashboardHeartbeatScrollPixels(
        tama.activity20ReceivedAt, now
      )
    : 0;
  uint8_t tokenIntensities[TOKEN_HEARTBEAT_SAMPLE_COUNT] = {};
  if (source == LANDSCAPE_HEARTBEAT_TOKEN) {
    tokenHeartbeatAgedIntensities(
      tama.tokenHeartbeat,
      now,
      tokenIntensities
    );
  }

  {
    const int16_t localCenterY =
      layout.heartbeatCenterY - layout.heartbeatY;
    landscapeHeartbeatSprite.fillSprite(LANDSCAPE_DASHBOARD_BG);
    landscapeHeartbeatSprite.drawFastHLine(
      0,
      localCenterY,
      layout.heartbeatWidth,
      LANDSCAPE_DASHBOARD_DIM
    );
    if (source == LANDSCAPE_HEARTBEAT_TOKEN) {
      landscapeDashboardDrawTokenHeartbeatCurve(
        landscapeHeartbeatSprite,
        tokenIntensities,
        0,
        localCenterY
      );
    } else if (source == LANDSCAPE_HEARTBEAT_ACTIVITY) {
      landscapeDashboardDrawHeartbeatCurve(
        landscapeHeartbeatSprite,
        activityMask,
        latestBucket,
        0,
        localCenterY,
        LANDSCAPE_DASHBOARD_GREEN,
        scrollPixels
      );
    }
    landscapeHeartbeatSprite.pushSprite(
      layout.heartbeatX,
      layout.heartbeatY
    );
    return;
  }
}

template <typename Canvas>
static void drawLandscapeDashboardProgressCell(
  Canvas& canvas,
  int16_t x,
  int16_t y,
  uint16_t color
) {
  canvas.fillTriangle(x + 3, y, x + 7, y, x, y + 6, color);
  canvas.fillTriangle(x + 7, y, x + 4, y + 6, x, y + 6, color);
}

template <typename Canvas>
static void drawLandscapeDashboardTime(
  Canvas& canvas,
  bool fieldsValid
) {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  char hm[6];
  char seconds[3];
  if (fieldsValid) {
    clockFormatHm(hm, sizeof(hm), _clkTm.Hours, _clkTm.Minutes);
    clockFormatSecondNumber(seconds, sizeof(seconds), _clkTm.Seconds);
  } else {
    snprintf(hm, sizeof(hm), "--:--");
    snprintf(seconds, sizeof(seconds), "--");
  }

  canvas.fillRect(layout.timeX, layout.timeY, 160, 40, LANDSCAPE_DASHBOARD_BG);
  useDashboardTimeFont(canvas);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(LANDSCAPE_DASHBOARD_IDLE, LANDSCAPE_DASHBOARD_BG);
  canvas.drawString(hm, layout.timeX, layout.timeY);

  const uint8_t litBlocks = fieldsValid
    ? landscapeDashboardSecondBlocks(_clkTm.Seconds)
    : 0;
  for (uint8_t i = 0; i < 4; ++i) {
    drawLandscapeDashboardProgressCell(
      canvas,
      layout.secondProgressX + (i * 8),
      layout.secondProgressY,
      i < litBlocks ? LANDSCAPE_DASHBOARD_IDLE : LANDSCAPE_DASHBOARD_DIM
    );
  }
  useDashboardSecondsFont(canvas);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(LANDSCAPE_DASHBOARD_SECONDS_TEXT, LANDSCAPE_DASHBOARD_BG);
  canvas.drawString(seconds, layout.secondsX, layout.secondsY);
}

template <typename Canvas>
static void drawLandscapeDashboardDate(
  Canvas& canvas,
  bool fieldsValid
) {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  char dateLine[8];
  clockFormatSharedDateLine(
    dateLine,
    sizeof(dateLine),
    false,
    fieldsValid,
    _clkDt.WeekDay,
    _clkDt.Month,
    _clkDt.Date
  );
  const char* weekday = fieldsValid
    ? clockWeekdayFullLabel(_clkDt.WeekDay)
    : "---";
  canvas.fillRect(4, layout.dateY, 160, 14, LANDSCAPE_DASHBOARD_BG);
  useDashboardStatusFont(canvas);
  canvas.setTextColor(LANDSCAPE_DASHBOARD_DATE_TEXT, LANDSCAPE_DASHBOARD_BG);
  canvas.setTextDatum(TL_DATUM);
  canvas.drawString(dateLine, 4, layout.dateY);
  canvas.setTextDatum(TR_DATUM);
  canvas.drawString(weekday, 164, layout.dateY);
  const int16_t weekdayWidth = canvas.textWidth(weekday);
  const int16_t lineRight = 164 - weekdayWidth - 8;
  if (lineRight > 63) {
    canvas.drawFastHLine(63, layout.dateY + 7, lineRight - 63, LANDSCAPE_DASHBOARD_DIM);
  }
}

template <typename Canvas>
static void drawLandscapeDashboardCards(
  Canvas& canvas,
  SharedClockStatusCounts statusCounts
) {
  const LandscapeDashboardLayout layout = landscapeDashboardLayout();
  static const char* labels[] = {"RUN", "ASK", "NEW"};
  const uint8_t counts[] = {
    statusCounts.running,
    statusCounts.waiting,
    statusCounts.unread,
  };
  const uint16_t colors[] = {
    LANDSCAPE_DASHBOARD_RUN,
    LANDSCAPE_DASHBOARD_ASK,
    LANDSCAPE_DASHBOARD_NEW,
  };
  const uint16_t tints[] = {
    LANDSCAPE_DASHBOARD_RUN_TINT,
    LANDSCAPE_DASHBOARD_ASK_TINT,
    LANDSCAPE_DASHBOARD_NEW_TINT,
  };
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t y = layout.cardsY[i];
    canvas.fillRoundRect(
      layout.cardsX,
      y,
      layout.cardsWidth,
      layout.cardHeight,
      3,
      tints[i]
    );
    canvas.fillRoundRect(layout.cardsX, y, 30, layout.cardHeight, 3, colors[i]);
    useDashboardCardFont(canvas);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextColor(0x0000, colors[i]);
    canvas.drawString(labels[i], layout.cardsX + 4, y + 1);

    char countText[4] = {};
    statusDashboardFormatCount(countText, sizeof(countText), counts[i]);
    useDashboardStatusFont(canvas);
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextColor(
      landscapeDashboardCountColor(counts[i], colors[i]),
      tints[i]
    );
    canvas.drawString(
      countText,
      layout.cardsX + 47,
      y + (layout.cardHeight / 2)
    );
  }
}

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
  const uint16_t surfaceBg = landscape ? LANDSCAPE_DASHBOARD_BG : p.bg;
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
  if (!landscape && !buddyMode) {
    characterSetPeek(true);
    characterSetState(activeState);
  }

  const uint32_t now = millis();
  const uint8_t meterAnimationFrame = usageMeterLandscapeAnimationFrame(now);
  SharedClockStatusCounts statusCounts = {
    tama.sessionsRunning,
    tama.sessionsWaiting,
    static_cast<uint8_t>(tama.hasUnreadCount ? tama.unreadCount : 0),
  };

  UsageMeterRenderFrame meterFrame = landscape
    ? usageMeterLandscapeFrameForDisplay(
        meterState,
        layout.screenWidth,
        layout.screenHeight,
        false,
        statusCounts.running > 0,
        meterAnimationFrame
      )
    : usageMeterFrameForDisplay(
        meterState, layout.screenWidth, layout.screenHeight, false
      );
  SharedClockFaceRenderDecision decision = clockSharedFaceSchedule(
    cache,
    now,
    orientation,
    layout.status.visible,
    statusCounts,
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
    canvas.fillScreen(surfaceBg);
    usageMeterRenderReset(meterState);
    meterFrame = landscape
      ? usageMeterLandscapeFrameForDisplay(
          meterState,
          layout.screenWidth,
          layout.screenHeight,
          true,
          statusCounts.running > 0,
          meterAnimationFrame
        )
      : usageMeterFrameForDisplay(
          meterState, layout.screenWidth, layout.screenHeight, true
        );
  } else if (meterFrame.decision.clear) {
    canvas.fillRect(
      0,
      layout.meterY,
      layout.screenWidth,
      layout.meterFootprint,
      surfaceBg
    );
  }

  // A functional screen may have selected the proportional Chinese font.
  // Restore the dedicated fixed ASCII face before applying pixel geometry.
  useSharedFaceAsciiFont(canvas);
  if (landscape) {
    drawLandscapeDashboardHeartbeat(canvas, now, decision.fullRepaint);
  }
  if (decision.drawTime) {
    if (landscape) {
      drawLandscapeDashboardTime(canvas, fieldsValid);
    } else {
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
      useDashboardSecondsFont(canvas);
      canvas.setTextSize(layout.time.primaryTextSize);
      canvas.setTextColor(p.text, p.bg);
      canvas.drawString(hm, layout.time.primary.x, layout.time.primary.y);
      if (layout.time.showSeconds) {
        useDashboardSecondsFont(canvas);
        canvas.setTextSize(layout.time.secondsTextSize);
        canvas.setTextColor(p.textDim, p.bg);
        canvas.drawString(seconds, layout.time.seconds.x, layout.time.seconds.y);
      }
    }
  }

  if (decision.drawDate) {
    if (landscape) {
      drawLandscapeDashboardDate(canvas, fieldsValid);
    } else if (layout.date.mode == SHARED_CLOCK_DATE_STACKED_MONTH_DAY) {
      char month[4];
      char day[3];
      if (fieldsValid) {
        snprintf(month, sizeof(month), "%s", clockMonthLabel(_clkDt.Month));
        snprintf(day, sizeof(day), "%02d", _clkDt.Date);
      } else {
        snprintf(month, sizeof(month), "---");
        snprintf(day, sizeof(day), "--");
      }
      canvas.setTextDatum(TC_DATUM);
      canvas.setTextColor(p.textDim, p.bg);
      canvas.setTextSize(layout.date.monthTextSize);
      canvas.drawString(
        month,
        sharedClockTextRectCenterX(layout.date.month),
        layout.date.month.y
      );
      canvas.setTextSize(layout.date.dayTextSize);
      canvas.drawString(
        day,
        sharedClockTextRectCenterX(layout.date.day),
        layout.date.day.y
      );
    } else {
      char dateLine[16];
      clockFormatSharedDateLine(
        dateLine,
        sizeof(dateLine),
        false,
        fieldsValid,
        _clkDt.WeekDay,
        _clkDt.Month,
        _clkDt.Date
      );
      useSharedFaceAsciiFont(canvas);
      canvas.setTextDatum(MC_DATUM);
      canvas.setTextSize(layout.date.monthTextSize);
      canvas.setTextColor(p.textDim, p.bg);
      canvas.drawString(dateLine, layout.date.centerX, layout.date.centerY);
    }
  }

  if (decision.drawStatus && layout.status.visible) {
    if (landscape) {
      drawLandscapeDashboardCards(canvas, statusCounts);
    } else {
      canvas.fillRect(
        layout.status.x,
        layout.status.y,
        layout.status.width,
        layout.status.height,
        p.bg
      );
      static const char* labels[] = {"RUN", "ASK", "NEW"};
      const uint8_t counts[] = {
        statusCounts.running,
        statusCounts.waiting,
        statusCounts.unread,
      };
      const StatusDashboardKind kinds[] = {STATUS_RUN, STATUS_ASK, STATUS_NEW};
      canvas.setTextDatum(MC_DATUM);
      for (uint8_t i = 0; i < 3; ++i) {
        const int16_t centerX = layout.status.x +
          (i * layout.status.columnWidth) + (layout.status.columnWidth / 2);
        canvas.setTextSize(STATUS_DASHBOARD_LABEL_TEXT_SIZE);
        canvas.setTextColor(p.textDim, p.bg);
        canvas.drawString(
          labels[i],
          centerX,
          statusDashboardLabelCenterY(layout.status.y)
        );
        char countText[4] = {};
        statusDashboardFormatCount(countText, sizeof(countText), counts[i]);
        canvas.setTextSize(STATUS_DASHBOARD_COUNT_TEXT_SIZE);
        canvas.setTextColor(
          statusDashboardColor(statusDashboardColorRole(kinds[i], counts[i]), p),
          p.bg
        );
        canvas.drawString(
          countText,
          centerX,
          statusDashboardCountCenterY(layout.status.y)
        );
      }
    }
  }

  if (decision.drawPet) {
    if (landscape) {
      drawLandscapeDashboardStatus(canvas);
    } else {
    lgfx::LovyanGFX* petCanvas = &canvas;
    int16_t petX = layout.pet.x;
    int16_t petY = layout.pet.y;
    bool renderPetContent = true;
    if (layout.pet.useLocalSurface) {
      if (landscapeClockPetSpriteReady) {
        petCanvas = &landscapeClockPetSprite;
        petX = 0;
        petY = 0;
        if (sharedClockPetLocalSurfaceNeedsClear(buddyMode, decision.fullRepaint)) {
          landscapeClockPetSprite.fillSprite(p.bg);
        }
      } else {
        canvas.fillRect(
          layout.pet.x,
          layout.pet.y,
          layout.pet.width,
          layout.pet.height,
          p.bg
        );
        renderPetContent = false;
      }
    }

    if (renderPetContent && buddyMode) {
      // ASCII species paint sparse glyphs and particles, so clear only the
      // shared compact pet rectangle. The time and usage regions stay intact.
      if (!layout.pet.useLocalSurface) {
        petCanvas->fillRect(petX, petY, layout.pet.width, layout.pet.height, p.bg);
      }
      buddyRenderTo(
        petCanvas,
        activeState,
        petX + layout.pet.width / 2,
        petY + layout.pet.asciiYOffset,
        layout.pet.asciiScale
      );
    } else if (renderPetContent) {
      // The compact character renderer routes both GIF and text manifests.
      // Text frames clear only this pet rectangle; GIF frames self-erase so
      // they avoid a black flash before each decoded scanline.
      characterRenderCompactTo(
        petCanvas,
        petX + layout.pet.width / 2,
        petY + layout.pet.height / 2,
        petX,
        petY,
        layout.pet.width,
        layout.pet.height,
        landscape ? 3 : 1,
        landscape ? 5 : 2,
        decision.fullRepaint
      );
    }
    if (layout.pet.useLocalSurface && landscapeClockPetSpriteReady) {
      landscapeClockPetSprite.pushSprite(layout.pet.x, layout.pet.y);
    }
    }
  }

  // Usage is always the final layer, so no pet/text refresh can cover it.
  if (meterFrame.decision.draw) {
    paintUsageMeter(
      canvas,
      meterFrame.plan,
      landscape && usageMeterLandscapeAnimationEnabled(
        meterFrame.plan,
        statusCounts.running
      ),
      meterAnimationFrame
    );
  }
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
  useUtf8FontForText(spr, "Info", &fonts::efontCN_12);
  spr.setCursor(4, y); spr.print("Info");
  useDefaultTextFont(spr);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 28, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += 12;
  spr.setTextColor(p.body, p.bg);
  useUtf8FontForText(spr, section, &fonts::efontCN_12);
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
    useUtf8FontForText(spr, line, &fonts::efontCN_12);
    spr.setCursor(4, y); spr.print(line);
    y += utf8ContainsNonAscii(line) ? 13 : 8;
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
    usageMeterBottomInset()
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
    spr.setFont(&fonts::efontCN_12);
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
  const int FOOTER_Y = landscapeApprovalFooterY(LH, usageMeterBottomInset());
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
  useUtf8FontForText(M5.Lcd, toolLine, &fonts::efontCN_12);
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
  M5.Lcd.setRotation(runtimeOrientationState.orientation);
  bool promptExited = runtimeNeedsFullRepaintOnPromptExit(
    runtimeLandscapePromptVisible,
    inPrompt
  );
  bool promptEntered = !runtimeLandscapePromptVisible && inPrompt;
  bool repaint = paintedRuntimeOrient != runtimeOrientationState.orientation || promptEntered || promptExited;
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
    paintedRuntimeOrient = runtimeOrientationState.orientation;
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
  // The rollback supervisor starts before every fallible board/application
  // initialiser. It remains completely inert unless the bootloader reports
  // that this exact running image is PENDING_VERIFY.
  otaBootHealthArmEarly(millis());

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
  uint32_t buttonProbeStart = millis();
  M5.update();
  uint32_t buttonAUpdated = M5.BtnA.getUpdateMsec();
  uint32_t buttonBUpdated = M5.BtnB.getUpdateMsec();
  bool buttonsReadable = buttonAUpdated == buttonBUpdated &&
    static_cast<int32_t>(buttonAUpdated - buttonProbeStart) >= 0 &&
    millis() - buttonAUpdated < 100;
  if (buttonsReadable) otaBootHealthReady(OTA_BOOT_READY_BUTTONS);
  else otaBootHealthCriticalFailure(OTA_BOOT_REASON_BUTTONS);
  otaBootHealthPoll(millis());

  startBt();
  if (bleReady()) otaBootHealthReady(OTA_BOOT_READY_BLE);
  else if (bleStartupFailed()) otaBootHealthCriticalFailure(OTA_BOOT_REASON_BLE);
  otaBootHealthPoll(millis());
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
  void* spriteBuffer = spr.createSprite(W, H);
  bool displayUsable = M5.getDisplayCount() > 0 && spriteBuffer &&
    M5.Lcd.width() == W && M5.Lcd.height() == H;
  if (displayUsable) otaBootHealthReady(OTA_BOOT_READY_DISPLAY);
  else otaBootHealthCriticalFailure(OTA_BOOT_REASON_DISPLAY);
  otaBootHealthPoll(millis());

  const SharedClockFaceLayout landscapeFace = sharedClockFaceLayout(true);
  landscapeClockPetSprite.setColorDepth(16);
  landscapeClockPetSpriteReady = landscapeClockPetSprite.createSprite(
    landscapeFace.pet.width,
    landscapeFace.pet.height
  ) != nullptr;
  if (!landscapeClockPetSpriteReady) {
    Serial.println("[display] landscape pet sprite allocation failed");
  }
  const LandscapeDashboardLayout dashboardLayout = landscapeDashboardLayout();
  landscapeHeartbeatSprite.setColorDepth(16);
  landscapeHeartbeatSpriteReady = landscapeHeartbeatSprite.createSprite(
    dashboardLayout.heartbeatWidth,
    dashboardLayout.heartbeatHeight
  ) != nullptr;
  if (!landscapeHeartbeatSpriteReady) {
    Serial.println("[display] landscape heartbeat sprite allocation failed");
  }

  characterInit(nullptr);  // scan /characters/ for whatever is installed
  // A missing custom character is healthy because the built-in ASCII buddy is
  // the explicit fallback. The filesystem itself must have mounted/recovered.
  bool storageUsable = LittleFS.totalBytes() > 0;
  if (storageUsable) otaBootHealthReady(OTA_BOOT_READY_STORAGE);
  else otaBootHealthCriticalFailure(OTA_BOOT_REASON_STORAGE);
  otaBootHealthPoll(millis());
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
  otaBootHealthLogStatus();
  otaBootHealthPoll(millis());
}

void loop() {
  // First safe point: timeouts and previously latched failures win before any
  // normal application work. A rollback path never returns.
  otaBootHealthPoll(millis());
  M5.update();
  // Advertising readiness is asynchronous. The GAP callback, rather than the
  // void startAdvertising() request, is the concrete health signal.
  if (bleReady()) otaBootHealthReady(OTA_BOOT_READY_BLE);
  else if (bleStartupFailed()) otaBootHealthCriticalFailure(OTA_BOOT_REASON_BLE);
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (completionChimeObserve(
        &completionChimeState,
        tama.hasCompletionSeq,
        tama.completionSeq
      )) {
    playCompletionSound();
  }
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
      otaCompactOverlay = false;
      otaUpdateCancel();
      otaOfferCancel(&tama.otaOffer);
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;
  wifiManagerPoll(inPrompt);
  OtaOfferPolicy incomingOtaPolicy = otaOfferPolicy(
    tama.otaOffer.pending,
    tama.otaOffer.signedAuthorized,
    settings().autoOta
  );
  if (incomingOtaPolicy.automatic) {
    // Ordinary navigation is safe to dismiss for an explicit Direct command.
    // Prompt/xfer/provisioning/passkey remain live inputs to the execution gate.
    displayMode = DISP_NORMAL;
    menuOpen = settingsOpen = wifiMenuOpen = wifiStatusOpen = resetOpen = false;
    otaReceiveScreen = false;
  }
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
  otaInputs.automaticPolicy = settings().autoOta;
  otaUpdatePoll(&tama.otaOffer, otaInputs);
  OtaUpdateView updateView = otaUpdateView();
  OtaUiPlan updateUiPlan = otaUiPlan(
    updateView.visible,
    updateView.phase == OTA_PHASE_CONFIRM,
    updateView.automatic,
    updateView.bootCommitted,
    updateView.cancellable
  );
  bool compactOverlayWasVisible = otaCompactOverlay;
  otaCompactOverlay = updateUiPlan.compactOverlay && !inPrompt;
  if (otaCompactOverlay) {
    otaReceiveScreen = false;
  } else if (updateView.visible && !otaReceiveScreen && !updateView.automatic) {
    // A verified signed offer is a deliberate Mac-side action. Bring its
    // confirmation/progress surface forward without requiring menu navigation.
    displayMode = DISP_NORMAL;
    menuOpen = settingsOpen = wifiMenuOpen = wifiStatusOpen = resetOpen = false;
    otaReceiveScreen = true;
    applyDisplayMode();
  }
  if (compactOverlayWasVisible && !otaCompactOverlay) {
    invalidatePortraitSurface();
    clockSharedFaceCacheReset(&standbyClockFaceCache);
    clockSharedFaceCacheReset(&runtimeClockFaceCache);
    usageMeterRenderReset(&clockUsageMeterRenderState);
    usageMeterRenderReset(&runtimeUsageMeterRenderState);
  }
  otaStatusPoll(updateView);
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
    otaBootHealthReady(OTA_BOOT_READY_EVENT_LOOP);
    otaBootHealthPoll(millis());
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
    if (otaCompactOverlay) {
      if (otaUpdateView().cancellable) otaUpdateCancel();
    }
    else if (otaReceiveScreen) {
      if (otaUpdateActive()) {
        if (otaUpdateView().cancellable) otaUpdateCancel();
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
      } else if (otaCompactOverlay) {
        // Automatic OTA is informational; A does not change the transaction.
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
    } else if (otaCompactOverlay) {
      beep(600, 40);
      if (otaUpdateView().cancellable) otaUpdateCancel();
    } else if (otaReceiveScreen) {
      beep(600, 40);
      if (otaUpdateActive()) {
        if (otaUpdateView().cancellable) otaUpdateCancel();
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
  sharedContext.otaProgressVisible = otaUpdateActive() && !otaCompactOverlay;
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
  ScreenOrientationRenderDecision clockSurfaceDecision = screenOrientAutoSurfaceDecision(
    &clockAutoSurfaceRenderState,
    clocking,
    clockOrientationState.resolved
  );
  if (clockSurfaceDecision.entered) clockOrientBeginAutoSurface(&clockOrientationState);
  if (clocking) clockUpdateOrient();
  bool clockSurfaceRenderable = clocking && clockOrientationState.resolved;
  bool landscapeClock = clockSurfaceRenderable && clockOrientationState.orientation != 0;

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
  ScreenOrientationRenderDecision runtimeSurfaceDecision = screenOrientAutoSurfaceDecision(
    &runtimeAutoSurfaceRenderState,
    runtimeOrienting,
    runtimeOrientationState.resolved
  );
  bool runtimeSurfaceModeChanged = screenOrientRuntimeModeChanged(
    previousPromptVisible, inPrompt, runtimeOrienting
  );
  if (runtimeSurfaceDecision.entered || runtimeSurfaceModeChanged) {
    clockOrientBeginAutoSurface(&runtimeOrientationState);
  }
  if (runtimeOrienting) runtimeUpdateOrient();
  bool runtimeSurfaceRenderable = runtimeOrienting && runtimeOrientationState.resolved;
  bool landscapeRuntime = runtimeSurfaceRenderable && runtimeOrientationState.orientation != 0;
  bool portraitSharedFace = (clockSurfaceRenderable && !landscapeClock)
    || (runtimeSurfaceRenderable && !landscapeRuntime);
  bool autoSurfaceAwaitingOrientation = (clocking && !clockSurfaceRenderable)
    || (runtimeOrienting && !runtimeSurfaceRenderable);

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

  if (autoSurfaceAwaitingOrientation) {
    // Keep the current LCD contents unchanged until the IMU pose resolves.
  } else {
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
        runtimeOrientationState.orientation,
        &runtimeClockFaceCache,
        &runtimeUsageMeterRenderState,
        false,
        promptExited
      );
    }
    if (otaCompactOverlay) {
      drawLandscapeOtaCompactOverlay(runtimeOrientationState.orientation, updateView);
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
      clockOrientationState.orientation,
      &standbyClockFaceCache,
      &clockUsageMeterRenderState
    );
    if (otaCompactOverlay) {
      drawLandscapeOtaCompactOverlay(clockOrientationState.orientation, updateView);
    }
  } else if (!napping && !screenOff) {
    if (clocking) {
      drawSharedClockFace(
        false,
        0,
        &standbyClockFaceCache,
        &clockUsageMeterRenderState
      );
      if (otaCompactOverlay) drawOtaCompactOverlayTo(spr, false, updateView);
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
      if (otaCompactOverlay) drawOtaCompactOverlayTo(spr, false, updateView);
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
      if (otaCompactOverlay) drawOtaCompactOverlayTo(spr, false, updateView);
      spr.pushSprite(0, 0);
    }
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

  // Last safe point: only now has one real event/render iteration completed.
  // This is the final required health bit, so a pending image cannot be marked
  // valid merely because setup() returned.
  otaBootHealthReady(OTA_BOOT_READY_EVENT_LOOP);
  otaBootHealthPoll(millis());
  delay(screenOff ? 100 : 16);
}
