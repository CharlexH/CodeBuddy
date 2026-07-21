#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ble_bridge.h"
#include "clock_time_logic.h"
#include "data_log_logic.h"
#include "firmware_version.h"
#include "ota_authorization_logic.h"
#include "ota_manifest.h"
#include "ota_manifest_logic.h"
#include "status_dashboard_json.h"
#include "utf8_text_logic.h"
#include "usage_meter_json.h"
#include "wifi_manager.h"
#include "trusted_time_logic.h"
#include "xfer.h"

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     hasUnreadCount;
  uint8_t  unreadCount;
  bool     recentlyCompleted;
  bool     hasCompletionSeq;
  uint32_t completionSeq;
  bool     hasActivity20;
  uint32_t activity20;
  uint32_t activity20ReceivedAt;
  uint32_t tokensToday;
  bool     hasFiveHourUsage;
  bool     hasSevenDayUsage;
  uint8_t  fiveHourRemaining;
  uint8_t  sevenDayRemaining;
  uint32_t lastUpdated;
  char     msg[128];
  bool     connected;
  char     lines[8][256];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[96];
  char     promptHint[256];
  OtaOfferState otaOffer;
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 10s over USB or BT
//   asleep → no data, all zeros, "No Codex connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs = 0;
static uint32_t _lastBtByteMs = 0;   // hasClient() lies; track actual BT traffic
static bool     _demoMode   = false;
static uint8_t  _demoIdx    = 0;
static uint32_t _demoNext   = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "bt" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }

static OtaAuthorizationReplayState _otaAuthorizationReplay =
  otaAuthorizationReplayInitial();

inline bool dataOtaExpectedDeviceName(char output[11]) {
  if (!output) return false;
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
    output[0] = 0;
    return false;
  }
  snprintf(output, 11, "Codex-%02X%02X", mac[4], mac[5]);
  return true;
}

static void _applyJson(const char* line, TamaState* out, bool trustedTransport) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    char diagnostic[64] = {};
    formatMalformedJsonLog(line ? strlen(line) : 0, diagnostic, sizeof(diagnostic));
    Serial.println(diagnostic);
    return;
  }
  if (xferCommand(doc)) {
    const char* cmd = doc["cmd"];
    if (xferCommandConflictsWithOta(cmd)) otaOfferCancel(&out->otaOffer);
    Serial.printf("[data] cmd=%s\n", cmd ? cmd : "(null)");
    _lastLiveMs = millis();
    return;
  }

  // An OTA offer is coordination metadata only. It cannot start Wi-Fi,
  // download bytes, or write flash. The signed manifest is authenticated by
  // ota_manifest.cpp before any field is later trusted.
  JsonVariant otaVariant = doc["ota_offer"];
  if (otaVariant.is<JsonVariantConst>()) {
    bool accepted = false;
    if (otaVariant.is<JsonObject>()) {
      JsonObject offer = otaVariant.as<JsonObject>();
      const char* version = offer["version"].is<const char*>()
        ? offer["version"].as<const char*>() : nullptr;
      const char* manifestUrl = offer["manifestUrl"].is<const char*>()
        ? offer["manifestUrl"].as<const char*>() : nullptr;
      const char* signatureUrl = offer["signatureUrl"].is<const char*>()
        ? offer["signatureUrl"].as<const char*>() : nullptr;
      const char* nonce = offer["nonce"].is<const char*>()
        ? offer["nonce"].as<const char*>() : nullptr;
      bool generationTyped = offer["generation"].is<uint32_t>() &&
        !offer["generation"].is<bool>();
      uint32_t generation = generationTyped
        ? offer["generation"].as<uint32_t>() : 0;
      bool sizeTyped = offer["sizeBytes"].is<uint32_t>() &&
        !offer["sizeBytes"].is<bool>();
      uint32_t sizeBytes = sizeTyped ? offer["sizeBytes"].as<uint32_t>() : 0;
      bool promptConflict = out->promptId[0] != 0 || !doc["prompt"].isNull();
      int batteryPercent = (compatBatteryVoltageMv() - 3200) / 10;
      if (batteryPercent < 0) batteryPercent = 0;
      if (batteryPercent > 100) batteryPercent = 100;
      bool externalPower = compatVbusVoltageMv() > 4000;
      bool exactLegacyShape = offer.size() == 6;
      bool exactSignedShape = otaAuthorizationSignedOfferShapeValid(offer.size());
      bool reportRejection = exactLegacyShape;
      if (exactLegacyShape) {
        accepted = nonce && generationTyped && version && manifestUrl &&
          signatureUrl && sizeTyped && otaOfferAcceptBoundHint(
            nonce,
            generation,
            version,
            sizeBytes,
            manifestUrl,
            signatureUrl,
            CODE_BUDDY_FIRMWARE_VERSION,
            millis(),
            bleConnected(),
            promptConflict,
            xferActive(),
            wifiManagerUiActive(),
            externalPower,
            static_cast<uint8_t>(batteryPercent),
            &out->otaOffer
          );
      } else if (exactSignedShape) {
        const char* device = offer["device"].is<const char*>()
          ? offer["device"].as<const char*>() : nullptr;
        const char* authorization = offer["authorization"].is<const char*>()
          ? offer["authorization"].as<const char*>() : nullptr;
        bool issuedAtTyped = offer["issuedAt"].is<uint32_t>() &&
          !offer["issuedAt"].is<bool>();
        bool expiresAtTyped = offer["expiresAt"].is<uint32_t>() &&
          !offer["expiresAt"].is<bool>();
        uint32_t issuedAt = issuedAtTyped ? offer["issuedAt"].as<uint32_t>() : 0;
        uint32_t expiresAt = expiresAtTyped ? offer["expiresAt"].as<uint32_t>() : 0;
        char expectedDevice[11] = {};
        time_t systemEpoch = time(nullptr);
        bool epochInRange = systemEpoch >= 0 &&
          static_cast<uint64_t>(systemEpoch) <= UINT32_MAX;
        OtaAuthorizationInput authorizationInput = {
          OTA_AUTHORIZATION_ACTION,
          device,
          expiresAt,
          generation,
          issuedAt,
          manifestUrl,
          nonce,
          signatureUrl,
          sizeBytes,
          version,
          authorization,
        };
        OtaAuthorizationResult authorizationResult = OTA_AUTHORIZATION_FIELD_INVALID;
        accepted = nonce && generationTyped && version && manifestUrl && signatureUrl &&
          sizeTyped && device && authorization && issuedAtTyped && expiresAtTyped &&
          dataOtaExpectedDeviceName(expectedDevice) && epochInRange &&
          otaAuthorizationVerifyThenAccept(
            authorizationInput,
            expectedDevice,
            wifiManagerSystemTimeTrusted(),
            static_cast<uint32_t>(systemEpoch),
            otaVerifyDetachedSignature,
            nullptr,
            &_otaAuthorizationReplay,
            CODE_BUDDY_FIRMWARE_VERSION,
            millis(),
            bleConnected(),
            promptConflict,
            xferActive(),
            wifiManagerUiActive(),
            externalPower,
            static_cast<uint8_t>(batteryPercent),
            &out->otaOffer,
            &authorizationResult
          );
        reportRejection = otaAuthorizationMayReportRejection(authorizationResult);
      }
      if (accepted) otaStatusBindOffer(out->otaOffer);
      else if (reportRejection && nonce && generationTyped)
        otaStatusReject(nonce, generation, "rejected");
    }
    if (!accepted) otaOfferReject(&out->otaOffer);
    Serial.println(accepted ? "[ota] offer accepted" : "[ota] offer rejected");
    _lastLiveMs = millis();
    return;
  }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}. The system clock must
  // receive raw UTC for TLS certificate validation. The display RTC keeps the
  // separately offset local epoch for the existing clock-face behavior.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    int64_t utcEpoch = (int64_t)t[0].as<uint32_t>();
    int32_t timezoneOffset = (int32_t)t[1];
    if (!trustedTimeInputSane(utcEpoch, timezoneOffset)) {
      Serial.println("[data] rejected invalid time sync");
      return;
    }
    ClockSyncEpochs epochs = clockSyncEpochs(
      utcEpoch,
      timezoneOffset
    );
    bool trusted = wifiManagerAcceptHostUtc(
      epochs.utc_epoch, timezoneOffset, trustedTransport
    );
    time_t local = (time_t)epochs.local_epoch;
    struct tm lt; gmtime_r(&local, &lt);
    RTC_TimeTypeDef tm((int8_t)lt.tm_hour, (int8_t)lt.tm_min, (int8_t)lt.tm_sec);
    RTC_DateTypeDef dt((int16_t)(lt.tm_year + 1900), (int8_t)(lt.tm_mon + 1),
                       (int8_t)lt.tm_mday, (int8_t)lt.tm_wday);
    M5.Rtc.SetTime(&tm);
    M5.Rtc.SetDate(&dt);
    extern uint32_t _clkLastRead;
    extern void clockOnTimeSync(int64_t local_epoch);
    _clkLastRead = 0;   // force re-read so _clkDt and _rtcValid agree
    clockOnTimeSync((int64_t)local);
    _rtcValid = true;
    Serial.println(trusted ? "[data] trusted time sync" : "[data] display-only time sync");
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  StatusDashboardCounts dashboardCounts = {
    out->hasUnreadCount,
    out->unreadCount,
  };
  statusDashboardApplyUnreadJson(doc["unread"], &dashboardCounts);
  out->hasUnreadCount = dashboardCounts.hasUnread;
  out->unreadCount = dashboardCounts.unread;
  JsonVariantConst completionSeq = doc["completion_seq"];
  if (completionSeq.is<uint32_t>() && !completionSeq.is<bool>()) {
    out->hasCompletionSeq = true;
    out->completionSeq = completionSeq.as<uint32_t>();
  }
  JsonVariantConst activity20 = doc["activity20"];
  if (activity20.is<uint32_t>() && !activity20.is<bool>()) {
    const uint32_t mask = activity20.as<uint32_t>();
    if (mask <= 0x000FFFFFUL) {
      out->hasActivity20 = true;
      out->activity20 = mask;
      out->activity20ReceivedAt = millis();
    }
  }
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;

  UsageMeterState usageState = {
    out->hasFiveHourUsage,
    out->hasSevenDayUsage,
    out->fiveHourRemaining,
    out->sevenDayRemaining,
  };
  usageMeterApplyJson(doc["usage"], &usageState);
  out->hasFiveHourUsage = usageState.hasFiveHour;
  out->hasSevenDayUsage = usageState.hasSevenDay;
  out->fiveHourRemaining = usageState.fiveHourRemaining;
  out->sevenDayRemaining = usageState.sevenDayRemaining;

  const char* m = doc["msg"];
  if (m) utf8CopyTruncate(out->msg, m);
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= 8) break;
      const char* s = v.as<const char*>();
      utf8CopyTruncate(out->lines[n], s ? s : "");
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    strncpy(out->promptId,   pid ? pid : "", sizeof(out->promptId)-1);   out->promptId[sizeof(out->promptId)-1]=0;
    utf8CopyTruncate(out->promptTool, pt ? pt : "");
    utf8CopyTruncate(out->promptHint, ph ? ph : "");
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  otaOfferLifecyclePoll(
    &out->otaOffer,
    millis(),
    bleConnected(),
    out->promptId[0] != 0,
    xferActive(),
    wifiManagerUiActive()
  );
  Serial.printf(
    "[data] snapshot total=%u running=%u waiting=%u prompt=%s msg=%.36s\n",
    out->sessionsTotal,
    out->sessionsRunning,
    out->sessionsWaiting,
    out->promptId[0] ? out->promptId : "-",
    out->msg
  );
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out, bool trustedTransport) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out, trustedTransport); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  _usbLine.feed(Serial, out, true);
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out, bleSecure());
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  otaOfferLifecyclePoll(
    &out->otaOffer,
    now,
    out->connected && bleConnected(),
    out->promptId[0] != 0,
    xferActive(),
    wifiManagerUiActive()
  );
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    out->hasActivity20=false; out->activity20=0; out->activity20ReceivedAt=now;
    UsageMeterState usageState = {
      out->hasFiveHourUsage,
      out->hasSevenDayUsage,
      out->fiveHourRemaining,
      out->sevenDayRemaining,
    };
    usageMeterClear(&usageState);
    out->hasFiveHourUsage = usageState.hasFiveHour;
    out->hasSevenDayUsage = usageState.hasSevenDay;
    out->fiveHourRemaining = usageState.fiveHourRemaining;
    out->sevenDayRemaining = usageState.sevenDayRemaining;
    utf8CopyTruncate(out->msg, "No Codex connected");
  }
}
