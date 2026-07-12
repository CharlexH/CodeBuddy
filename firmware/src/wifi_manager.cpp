#include "wifi_manager.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <time.h>

namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
constexpr uint32_t SNTP_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr time_t TRUSTED_EPOCH_MIN = 1700000000;
constexpr uint8_t MAX_SCAN_RESULTS = 12;

Preferences prefs;
DNSServer dns;
WebServer portal(80);
WifiRuntimeState runtime = wifiInitialState(false);
bool portalActive = false;
bool handlersInstalled = false;
bool connectingPendingCredentials = false;
uint32_t connectDeadlineMs = 0;
uint32_t hostUtcAtMs = 0;
uint32_t nextSntpAttemptMs = 0;
char savedSsid[33] = "";
char savedPassword[64] = "";
char pendingSsid[33] = "";
char pendingPassword[64] = "";
char apSsid[24] = "";
char apPassword[16] = "";
char formNonce[17] = "";
char statusMessage[48] = "";

void safeCopy(char* dst, size_t dstSize, const char* src) {
  if (!dst || dstSize == 0) return;
  if (!src) src = "";
  size_t n = strnlen(src, dstSize - 1);
  memcpy(dst, src, n);
  dst[n] = 0;
}

void randomText(char* dst, size_t chars, const char* alphabet) {
  size_t alphabetLen = strlen(alphabet);
  for (size_t i = 0; i < chars; ++i) dst[i] = alphabet[esp_random() % alphabetLen];
  dst[chars] = 0;
}

bool requesterIsOnProvisioningAp() {
  return portal.client().localIP() == WiFi.softAPIP();
}

void sendPortalDenied() {
  portal.send(403, "text/plain", "Provisioning AP only");
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

void handleRoot() {
  if (!requesterIsOnProvisioningAp()) return sendPortalDenied();
  portal.send(200, "text/html", portalPage(statusMessage));
}

void handleSave() {
  if (!requesterIsOnProvisioningAp()) return sendPortalDenied();
  if (portal.clientContentLength() < 1 || portal.clientContentLength() > 512 ||
      !portal.hasArg("nonce") || !portal.hasArg("ssid") || !portal.hasArg("password") ||
      portal.arg("nonce").length() != 16 || portal.arg("nonce") != formNonce ||
      portal.arg("ssid").length() > 32 || portal.arg("password").length() > 63) {
    portal.send(400, "text/html", portalPage("Invalid or expired form. Please retry."));
    return;
  }
  String ssid = portal.arg("ssid");
  String password = portal.arg("password");
  if (!wifiSsidValid(ssid.c_str()) || !wifiPasswordValid(password.c_str())) {
    portal.send(400, "text/html", portalPage("Check the network name and password length."));
    return;
  }

  safeCopy(pendingSsid, sizeof(pendingSsid), ssid.c_str());
  safeCopy(pendingPassword, sizeof(pendingPassword), password.c_str());
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
  portal.send(202, "text/html",
              "<!doctype html><meta name=viewport content='width=device-width'>"
              "<p>Connecting CodeBuddy... Check the device screen.</p>");
}

void handleNotFound() {
  if (!requesterIsOnProvisioningAp()) return sendPortalDenied();
  portal.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  portal.send(302, "text/plain", "");
}

void installHandlers() {
  if (handlersInstalled) return;
  portal.on("/", HTTP_GET, handleRoot);
  portal.on("/save", HTTP_POST, handleSave);
  portal.on("/generate_204", HTTP_ANY, handleRoot);
  portal.on("/hotspot-detect.html", HTTP_ANY, handleRoot);
  portal.on("/ncsi.txt", HTTP_ANY, handleRoot);
  portal.onNotFound(handleNotFound);
  handlersInstalled = true;
}

void stopPortal() {
  if (!portalActive) return;
  dns.stop();
  portal.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  apPassword[0] = 0;
  formNonce[0] = 0;
  runtime.provisioningDeadlineMs = 0;
}

void beginSavedConnection() {
  if (!runtime.provisioned || !savedSsid[0]) return;
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(savedSsid, savedPassword);
  runtime.phase = WIFI_CONNECTING;
  connectingPendingCredentials = false;
  connectDeadlineMs = millis() + CONNECT_TIMEOUT_MS;
  safeCopy(statusMessage, sizeof(statusMessage), "Connecting to saved Wi-Fi");
}

void persistPendingCredentials() {
  prefs.begin("codebuddy_wifi", false);
  prefs.putString("ssid", pendingSsid);
  prefs.putString("password", pendingPassword);
  prefs.end();
  safeCopy(savedSsid, sizeof(savedSsid), pendingSsid);
  safeCopy(savedPassword, sizeof(savedPassword), pendingPassword);
  pendingSsid[0] = 0;
  pendingPassword[0] = 0;
}

void maybeStartSntp(uint32_t now) {
  if (runtime.phase != WIFI_ONLINE || wifiTrustedHostTimeFresh(hostUtcAtMs, now) ||
      (int32_t)(now - nextSntpAttemptMs) < 0) return;
  // TLS validation still uses the normal system trust store. SNTP only gives
  // the system clock a trusted UTC baseline; it never disables certificate checks.
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  nextSntpAttemptMs = now + SNTP_RETRY_MS;
}

}  // namespace

void wifiManagerBegin(const char* deviceSuffix) {
  prefs.begin("codebuddy_wifi", true);
  prefs.getString("ssid", savedSsid, sizeof(savedSsid));
  prefs.getString("password", savedPassword, sizeof(savedPassword));
  prefs.end();
  runtime = wifiInitialState(wifiSsidValid(savedSsid) && wifiPasswordValid(savedPassword));
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
    safeCopy(statusMessage, sizeof(statusMessage), "Could not start setup hotspot");
    runtime = wifiConnectFailed(runtime, millis(), esp_random());
    return false;
  }
  WiFi.scanNetworks(true, false, false, 300, 0);
  installHandlers();
  dns.start(DNS_PORT, "*", WiFi.softAPIP());
  portal.begin();
  portalActive = true;
  runtime = wifiPhysicalStart(runtime, millis());
  safeCopy(statusMessage, sizeof(statusMessage), "Join the hotspot, then open a browser");
  return true;
}

void wifiManagerCancelProvisioning() {
  bool hadSaved = runtime.provisioned;
  stopPortal();
  pendingSsid[0] = 0;
  pendingPassword[0] = 0;
  connectingPendingCredentials = false;
  runtime = wifiProvisioningCancel(runtime);
  if (hadSaved) beginSavedConnection();
  else WiFi.mode(WIFI_OFF);
}

void wifiManagerForget() {
  stopPortal();
  prefs.begin("codebuddy_wifi", false);
  prefs.clear();
  prefs.end();
  savedSsid[0] = savedPassword[0] = 0;
  pendingSsid[0] = pendingPassword[0] = 0;
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
    portal.handleClient();
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
    if (connectingPendingCredentials) persistPendingCredentials();
    runtime = wifiConnectSucceeded(runtime);
    connectingPendingCredentials = false;
    safeCopy(statusMessage, sizeof(statusMessage), "Connected");
    stopPortal();
  } else if (runtime.phase == WIFI_CONNECTING &&
             (int32_t)(now - connectDeadlineMs) >= 0) {
    WiFi.disconnect(false, false);
    safeCopy(statusMessage, sizeof(statusMessage), "Connection failed. Check password.");
    runtime = wifiConnectFailed(runtime, now, esp_random());
    connectingPendingCredentials = false;
    pendingSsid[0] = pendingPassword[0] = 0;
  } else if (!portalActive && wifiReconnectWorkDue(runtime, now, approvalPromptVisible)) {
    runtime = wifiRetryConnecting(runtime);
    beginSavedConnection();
  } else if (runtime.phase == WIFI_ONLINE && WiFi.status() != WL_CONNECTED) {
    runtime = wifiConnectFailed(runtime, now, esp_random());
    safeCopy(statusMessage, sizeof(statusMessage), "Wi-Fi disconnected");
  }
  maybeStartSntp(now);
}

void wifiManagerNoteHostUtc(uint32_t epochSeconds) {
  if (epochSeconds >= (uint32_t)TRUSTED_EPOCH_MIN) hostUtcAtMs = millis();
}

bool wifiManagerSystemTimeTrusted() {
  return wifiTrustedHostTimeFresh(hostUtcAtMs, millis()) || time(nullptr) >= TRUSTED_EPOCH_MIN;
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
