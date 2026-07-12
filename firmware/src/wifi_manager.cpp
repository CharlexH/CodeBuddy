#include "wifi_manager.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>
#include "wifi_credentials_logic.h"
#include "trusted_time_logic.h"
#include "wifi_portal_http_logic.h"

namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t SNTP_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr uint8_t MAX_SCAN_RESULTS = 12;

class PreferencesCredentialStore : public WifiCredentialStore {
 public:
  bool begin(bool readOnly) override { return prefs_.begin("codebuddy_wifi", readOnly); }
  void end() override { prefs_.end(); }
  bool putString(const char* key, const char* value) override {
    return prefs_.putString(key, value) == strlen(value);
  }
  bool getString(const char* key, char* out, size_t size) override {
    if (!prefs_.isKey(key)) return false;
    size_t n = prefs_.getString(key, out, size);
    return n > 0 && n < size;
  }
  bool putUInt(const char* key, uint32_t value) override {
    return prefs_.putUInt(key, value) == sizeof(value);
  }
  bool getUInt(const char* key, uint32_t* value) override {
    if (!value || !prefs_.isKey(key)) return false;
    *value = prefs_.getUInt(key, 0);
    return true;
  }
  bool putUChar(const char* key, uint8_t value) override {
    return prefs_.putUChar(key, value) == sizeof(value);
  }
  bool getUChar(const char* key, uint8_t* value) override {
    if (!value || !prefs_.isKey(key)) return false;
    *value = prefs_.getUChar(key, 0xff);
    return true;
  }
  bool remove(const char* key) override {
    return !prefs_.isKey(key) || prefs_.remove(key);
  }

 private:
  Preferences prefs_;
};

PreferencesCredentialStore credentialStore;
DNSServer dns;
WiFiServer portal(80, 1);
WiFiClient portalClient;
WifiHttpParser portalParser = {};
WifiRuntimeState runtime = wifiInitialState(false);
bool portalActive = false;
bool connectingPendingCredentials = false;
uint32_t connectDeadlineMs = 0;
uint32_t nextSntpAttemptMs = 0;
bool sntpRequested = false;
char savedSsid[33] = "";
char pendingSsid[33] = "";
char pendingPassword[64] = "";
char apSsid[24] = "";
char apPassword[16] = "";
char formNonce[17] = "";
char statusMessage[48] = "";
WifiCredentials activeCredentials = {};
TrustedTimeState trustedTime = trustedTimeInitial();

void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  size_t n = strnlen(src, dstSize - 1);
  memcpy(dst, src, n);
  dst[n] = 0;
}

void randomText(char* dst, size_t chars, const char* alphabet) {
  wifiSecureZero(dst, chars + 1);
  size_t alphabetLen = strlen(alphabet);
  for (size_t i = 0; i < chars; ++i) dst[i] = alphabet[esp_random() % alphabetLen];
  dst[chars] = 0;
}

bool requesterIsOnProvisioningAp(const WiFiClient& client) {
  return client.localIP() == WiFi.softAPIP();
}

void sendHttp(WiFiClient& client, uint16_t status, const char* type, const String& content) {
  const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted"
      : status == 400 ? "Bad Request" : status == 403 ? "Forbidden"
      : status == 405 ? "Method Not Allowed" : status == 408 ? "Request Timeout"
      : status == 413 ? "Payload Too Large" : status == 415 ? "Unsupported Media Type"
      : status == 431 ? "Request Header Fields Too Large" : "Error";
  client.printf("HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                status, reason, type, (unsigned)content.length());
  client.print(content);
}

void closePortalClient() {
  wifiSecureZero(portalParser.body, sizeof(portalParser.body));
  if (portalClient) portalClient.stop();
  portalClient = WiFiClient();
  wifiHttpParserReset(&portalParser, millis());
}

void clearString(String& value) {
  if (value.length()) wifiSecureZero(value.begin(), value.length());
  value = "";
}

String portalPage(const char* notice = nullptr) {
  String html;
  html.reserve(4096);
  html += F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>CodeBuddy Wi-Fi</title><style>body{font:16px system-ui;max-width:32rem;"
            "margin:2rem auto;padding:0 1rem;background:#07110d;color:#e7fff2}"
            "input,select,button{box-sizing:border-box;width:100%;padding:.75rem;margin:.35rem 0}"
            "button{background:#52e889;border:0;font-weight:700}</style><h1>CodeBuddy Wi-Fi</h1>");
  if (notice && notice[0]) {
    char escaped[192]; wifiHtmlEscape(notice, escaped, sizeof(escaped));
    html += F("<p>"); html += escaped; html += F("</p>");
  }
  html += F("<form method=post action=/save><input type=hidden name=nonce value='");
  html += formNonce;
  html += F("'><label>2.4 GHz network<input name=ssid list=networks maxlength=32 required>"
            "<datalist id=networks>");
  int count = WiFi.scanComplete();
  if (count < 0) count = 0;
  if (count > MAX_SCAN_RESULTS) count = MAX_SCAN_RESULTS;
  for (int i = 0; i < count; ++i) {
    char escaped[196]; wifiHtmlEscape(WiFi.SSID(i).c_str(), escaped, sizeof(escaped));
    html += F("<option value=\""); html += escaped; html += F("\">");
  }
  html += F("</datalist></label><label>Wi-Fi password<input name=password type=password "
            "maxlength=63 autocomplete=current-password></label><button>Connect</button></form>"
            "<p>Open networks may leave the password blank.</p>");
  return html;
}

void handlePortalComplete() {
  if (portalParser.method == WIFI_HTTP_GET) {
    String page = portalPage(statusMessage);
    sendHttp(portalClient, 200, "text/html", page);
    clearString(page);
    return;
  }
  WifiPortalForm form = {};
  bool valid = wifiHttpDecodeForm(portalParser.body, portalParser.bodyLength, &form) &&
      strlen(form.nonce) == 16 && strcmp(form.nonce, formNonce) == 0 &&
      wifiSsidValid(form.ssid) && wifiPasswordValid(form.password);
  if (!valid) {
    String page = portalPage("Invalid or expired form. Please retry.");
    sendHttp(portalClient, 400, "text/html", page);
    clearString(page);
    wifiSecureZero(&form, sizeof(form));
    return;
  }

  safeCopy(pendingSsid, sizeof(pendingSsid), form.ssid);
  safeCopy(pendingPassword, sizeof(pendingPassword), form.password);
  wifiSecureZero(&form, sizeof(form));
  wifiSecureZero(portalParser.body, sizeof(portalParser.body));
  // Consume the form nonce immediately. A failed connection gets a fresh form
  // from the still-running portal, while replaying this POST is rejected.
  randomText(formNonce, 16, "0123456789abcdef");
  WiFi.scanDelete();
  connectingPendingCredentials = true;
  runtime.phase = WIFI_CONNECTING;
  connectDeadlineMs = millis() + CONNECT_TIMEOUT_MS;
  safeCopy(statusMessage, sizeof(statusMessage), "Connecting. Keep this page open...");
  WiFi.persistent(false);
  WiFi.begin(pendingSsid, pendingPassword);
  sendHttp(portalClient, 202, "text/html",
           String("<!doctype html><meta name=viewport content='width=device-width'>"
                  "<p>Connecting CodeBuddy... Check the device screen.</p>"));
}

void pollPortalHttp(uint32_t now) {
  if (!portalClient || !portalClient.connected()) {
    if (portalClient) closePortalClient();
    WiFiClient candidate = portal.accept();
    if (candidate) {
      portalClient = candidate;
      wifiHttpParserReset(&portalParser, now);
      if (!requesterIsOnProvisioningAp(portalClient)) {
        sendHttp(portalClient, 403, "text/plain", String("Provisioning AP only"));
        closePortalClient();
        return;
      }
    }
  }
  if (!portalClient || !portalClient.connected()) return;
  size_t processed = 0;
  while (processed < WIFI_HTTP_POLL_BYTE_CAP && portalClient.available() &&
         portalParser.phase != WIFI_HTTP_COMPLETE &&
         portalParser.phase != WIFI_HTTP_REJECTED) {
    uint8_t byte = (uint8_t)portalClient.read();
    wifiHttpParserFeed(&portalParser, &byte, 1, 1, now);
    ++processed;
  }
  if (wifiHttpParserTimedOut(portalParser, now)) wifiHttpReject(&portalParser, 408);
  if (portalParser.phase == WIFI_HTTP_COMPLETE) {
    handlePortalComplete();
    closePortalClient();
  } else if (portalParser.phase == WIFI_HTTP_REJECTED) {
    sendHttp(portalClient, portalParser.statusCode, "text/plain", String("Request rejected"));
    closePortalClient();
  }
}

void cleanupProvisioningTransport() {
  dns.stop();
  closePortalClient();
  portal.end();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  wifiSecureZero(apPassword, sizeof(apPassword));
  wifiSecureZero(formNonce, sizeof(formNonce));
  wifiSecureZero(pendingSsid, sizeof(pendingSsid));
  wifiSecureZero(pendingPassword, sizeof(pendingPassword));
  connectingPendingCredentials = false;
  connectDeadlineMs = 0;
  runtime.provisioningDeadlineMs = 0;
}

void beginSavedConnection() {
  if (!runtime.provisioned || !activeCredentials.ssid[0]) return;
  WifiCredentials connection = {};
  bool loaded = activeCredentials.slot < 2
      ? wifiCredentialLoad(credentialStore, &connection) : false;
  if (!loaded && activeCredentials.slot == 0xff &&
      wifiCredentialMayUseLegacy(credentialStore)) {
    Preferences legacy;
    if (legacy.begin("codebuddy_wifi", true)) {
      bool keysPresent = legacy.isKey("ssid") && legacy.isKey("password");
      size_t sn = legacy.getString("ssid", connection.ssid, sizeof(connection.ssid));
      size_t pn = legacy.getString("password", connection.password, sizeof(connection.password));
      legacy.end();
      loaded = keysPresent && sn > 0 && pn < sizeof(connection.password) &&
        wifiSsidValid(connection.ssid) && wifiPasswordValid(connection.password);
    }
  }
  if (!loaded) {
    runtime = wifiInitialState(false);
    wifiSecureZero(&connection, sizeof(connection));
    WiFi.mode(WIFI_OFF);
    safeCopy(statusMessage, sizeof(statusMessage), "Saved Wi-Fi unavailable");
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(connection.ssid, connection.password);
  wifiSecureZero(connection.password, sizeof(connection.password));
  wifiSecureZero(&connection, sizeof(connection));
  runtime.phase = WIFI_CONNECTING;
  connectingPendingCredentials = false;
  connectDeadlineMs = millis() + CONNECT_TIMEOUT_MS;
  safeCopy(statusMessage, sizeof(statusMessage), "Connecting to saved Wi-Fi");
}

bool persistPendingCredentials() {
  WifiCredentials candidate = {};
  safeCopy(candidate.ssid, sizeof(candidate.ssid), pendingSsid);
  safeCopy(candidate.password, sizeof(candidate.password), pendingPassword);
  candidate.valid = true;
  WifiCredentials committed = {};
  bool saved = wifiCredentialCommit(
    credentialStore, activeCredentials, candidate, &committed
  );
  if (saved) {
    activeCredentials = committed;
    safeCopy(savedSsid, sizeof(savedSsid), committed.ssid);
    wifiSecureZero(activeCredentials.password, sizeof(activeCredentials.password));
  }
  wifiSecureZero(&candidate, sizeof(candidate));
  wifiSecureZero(&committed, sizeof(committed));
  wifiSecureZero(pendingSsid, sizeof(pendingSsid));
  wifiSecureZero(pendingPassword, sizeof(pendingPassword));
  return saved;
}

void maybeStartSntp(uint32_t now) {
  if (runtime.phase != WIFI_ONLINE) return;
  if (sntpRequested && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
    time_t epoch = time(nullptr);
    if (trustedTimeAcceptSntp(&trustedTime, (int64_t)epoch, true, now)) {
      sntpRequested = false;
      return;
    }
  }
  bool recentSecureHost = trustedTime.source == TRUSTED_TIME_SECURE_HOST &&
    (uint32_t)(now - trustedTime.syncAtMs) <= WIFI_HOST_UTC_FRESH_MS;
  if (recentSecureHost ||
      (trustedTime.source == TRUSTED_TIME_SNTP && trustedTimeFresh(trustedTime, now)) ||
      (int32_t)(now - nextSntpAttemptMs) < 0) return;
  // TLS validation still uses the normal system trust store. SNTP only gives
  // the system clock a trusted UTC baseline; it never disables certificate checks.
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  sntpRequested = true;
  nextSntpAttemptMs = now + SNTP_RETRY_MS;
}

}  // namespace

void wifiManagerBegin(const char* deviceSuffix) {
  WifiCredentials loaded = {};
  activeCredentials = {};
  savedSsid[0] = 0;
  if (wifiCredentialLoad(credentialStore, &loaded) &&
      wifiSsidValid(loaded.ssid) && wifiPasswordValid(loaded.password)) {
    activeCredentials = loaded;
    safeCopy(savedSsid, sizeof(savedSsid), loaded.ssid);
    wifiSecureZero(activeCredentials.password, sizeof(activeCredentials.password));
    wifiSecureZero(&loaded, sizeof(loaded));
  } else {
    // One-time compatibility path for firmware that stored the original two
    // unversioned keys. The first successful change migrates to the slots.
    if (wifiCredentialMayUseLegacy(credentialStore)) {
      Preferences legacy;
      char legacyPassword[64] = {};
      bool keysPresent = false;
      if (legacy.begin("codebuddy_wifi", true)) {
        keysPresent = legacy.isKey("ssid") && legacy.isKey("password");
        legacy.getString("ssid", savedSsid, sizeof(savedSsid));
        legacy.getString("password", legacyPassword, sizeof(legacyPassword));
        legacy.end();
      }
      if (keysPresent && wifiSsidValid(savedSsid) && wifiPasswordValid(legacyPassword)) {
        safeCopy(activeCredentials.ssid, sizeof(activeCredentials.ssid), savedSsid);
        activeCredentials.slot = 0xff;
        activeCredentials.generation = 0;
        activeCredentials.valid = true;
      }
      wifiSecureZero(legacyPassword, sizeof(legacyPassword));
    } else {
      safeCopy(statusMessage, sizeof(statusMessage), "Wi-Fi metadata invalid");
    }
  }
  runtime = wifiInitialState(activeCredentials.valid);
  snprintf(apSsid, sizeof(apSsid), "CodeBuddy-%s", deviceSuffix ? deviceSuffix : "SETUP");
  if (runtime.provisioned) beginSavedConnection();
  else WiFi.mode(WIFI_OFF);
}

bool wifiManagerStartProvisioning() {
  if (portalActive) return true;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  randomText(apPassword, 12, "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789");
  randomText(formNonce, 16, "0123456789abcdef");
  if (!WiFi.softAP(apSsid, apPassword, 1, false, 1)) {
    bool restoreSaved = runtime.provisioned;
    cleanupProvisioningTransport();
    runtime = wifiProvisioningStartFailed(runtime);
    if (restoreSaved) beginSavedConnection();
    else WiFi.mode(WIFI_OFF);
    safeCopy(statusMessage, sizeof(statusMessage), "Setup hotspot failed. Retry.");
    return false;
  }
  WiFi.scanNetworks(true, false, false, 300, 0);
  dns.start(DNS_PORT, "*", WiFi.softAPIP());
  portal.begin();
  portalActive = true;
  runtime = wifiPhysicalStart(runtime, millis());
  safeCopy(statusMessage, sizeof(statusMessage), "Join the hotspot, then open a browser");
  return true;
}

void wifiManagerCancelProvisioning() {
  bool hadSaved = runtime.provisioned;
  cleanupProvisioningTransport();
  runtime = wifiProvisioningCancel(runtime);
  if (hadSaved) beginSavedConnection();
  else WiFi.mode(WIFI_OFF);
}

void wifiManagerForget() {
  cleanupProvisioningTransport();
  Preferences cleanup;
  if (cleanup.begin("codebuddy_wifi", false)) {
    cleanup.clear();
    cleanup.end();
  }
  activeCredentials = {};
  wifiSecureZero(savedSsid, sizeof(savedSsid));
  wifiSecureZero(pendingSsid, sizeof(pendingSsid));
  wifiSecureZero(pendingPassword, sizeof(pendingPassword));
  connectingPendingCredentials = false;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  runtime = wifiForget(runtime);
  safeCopy(statusMessage, sizeof(statusMessage), "Wi-Fi forgotten");
}

void wifiManagerPoll(bool approvalPromptVisible) {
  uint32_t now = millis();
  if (portalActive) {
    dns.processNextRequest();
    pollPortalHttp(now);
    if (wifiProvisioningTimedOut(runtime, now)) {
      safeCopy(statusMessage, sizeof(statusMessage), "Setup timed out");
      wifiManagerCancelProvisioning();
      return;
    }
  }

  if (approvalPromptVisible && !portalActive && runtime.provisioned &&
      runtime.phase == WIFI_CONNECTING) {
    WiFi.disconnect(false, false);
    runtime = wifiConnectFailed(runtime, now, esp_random());
    safeCopy(statusMessage, sizeof(statusMessage), "Reconnect paused for approval");
    return;
  }

  if (runtime.phase == WIFI_CONNECTING && WiFi.status() == WL_CONNECTED) {
    if (connectingPendingCredentials && !persistPendingCredentials()) {
      connectingPendingCredentials = false;
      runtime = wifiConnectFailed(runtime, now, esp_random());
      safeCopy(statusMessage, sizeof(statusMessage), "Save failed. Re-enter Wi-Fi.");
      return;
    }
    runtime = wifiConnectSucceeded(runtime);
    connectingPendingCredentials = false;
    safeCopy(statusMessage, sizeof(statusMessage), "Connected");
    cleanupProvisioningTransport();
  } else if (runtime.phase == WIFI_CONNECTING &&
             (int32_t)(now - connectDeadlineMs) >= 0) {
    WiFi.disconnect(false, false);
    safeCopy(statusMessage, sizeof(statusMessage), "Connection failed. Check password.");
    runtime = wifiConnectFailed(runtime, now, esp_random());
    connectingPendingCredentials = false;
    wifiSecureZero(pendingSsid, sizeof(pendingSsid));
    wifiSecureZero(pendingPassword, sizeof(pendingPassword));
  } else if (!portalActive && wifiReconnectWorkDue(runtime, now, approvalPromptVisible)) {
    runtime = wifiRetryConnecting(runtime);
    beginSavedConnection();
  } else if (runtime.phase == WIFI_ONLINE && WiFi.status() != WL_CONNECTED) {
    runtime = wifiConnectFailed(runtime, now, esp_random());
    safeCopy(statusMessage, sizeof(statusMessage), "Wi-Fi disconnected");
  }
  maybeStartSntp(now);
}

bool wifiManagerAcceptHostUtc(
  int64_t epochSeconds,
  int32_t timezoneOffset,
  bool authenticated
) {
  TrustedTimeState candidate = trustedTime;
  if (!trustedTimeAcceptHost(
        &candidate, epochSeconds, timezoneOffset, authenticated, millis())) return false;
  struct timeval tv = {(time_t)epochSeconds, 0};
  if (settimeofday(&tv, nullptr) != 0) return false;
  trustedTime = candidate;
  return true;
}

bool wifiManagerSystemTimeTrusted() {
  return trustedTimeFresh(trustedTime, millis());
}

bool wifiManagerOnline() { return runtime.phase == WIFI_ONLINE && WiFi.status() == WL_CONNECTED; }
bool wifiManagerProvisioned() { return runtime.provisioned; }
bool wifiManagerUiActive() { return portalActive; }

WifiManagerSnapshot wifiManagerSnapshot() {
  WifiManagerSnapshot out = {};
  out.runtime = runtime;
  out.portalActive = portalActive;
  out.systemTimeTrusted = wifiManagerSystemTimeTrusted();
  out.rssi = wifiManagerOnline() ? WiFi.RSSI() : 0;
  wifiStatusClip(wifiManagerOnline() ? WiFi.SSID().c_str() : savedSsid,
                 out.ssid, sizeof(out.ssid));
  wifiStatusClip(wifiManagerOnline() ? WiFi.localIP().toString().c_str() : "-",
                 out.ip, sizeof(out.ip));
  wifiStatusClip(apSsid, out.apSsid, sizeof(out.apSsid));
  wifiStatusClip(apPassword, out.apPassword, sizeof(out.apPassword));
  wifiStatusClip(statusMessage, out.message, sizeof(out.message));
  return out;
}
