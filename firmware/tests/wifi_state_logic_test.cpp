#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_state_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "%s\n", message);
    exit(1);
  }
}

int main() {
  WifiRuntimeState state = wifiInitialState(false);
  expect_true(state.phase == WIFI_UNPROVISIONED, "fresh device should be unprovisioned");

  state = wifiPhysicalStart(state, 1000);
  expect_true(state.phase == WIFI_PROVISIONING, "physical action should start provisioning");
  expect_true(state.provisioningDeadlineMs == 601000,
              "provisioning should expire after at most ten minutes");
  expect_true(wifiProvisioningTimedOut(state, 601000), "deadline should be inclusive");
  state = wifiProvisioningCancel(state);
  expect_true(state.phase == WIFI_UNPROVISIONED, "cancel should return fresh device to unprovisioned");

  state = wifiInitialState(true);
  expect_true(state.phase == WIFI_CONNECTING, "saved credentials should reconnect on boot");
  state = wifiConnectSucceeded(state);
  expect_true(state.phase == WIFI_ONLINE && state.provisioned,
              "connect success should mark saved network online");
  state = wifiPhysicalStart(state, 50);
  state.phase = WIFI_CONNECTING;
  expect_true(wifiProvisioningTimedOut(state, state.provisioningDeadlineMs),
              "ten-minute cap should include an in-flight connection");
  state = wifiConnectFailed(state, 100, 7);
  expect_true(state.phase == WIFI_ERROR && state.provisioned,
              "failed reprovision should retain the previously saved network");
  expect_true(state.retryAtMs > 100, "failure should schedule a bounded retry");
  expect_true(!wifiReconnectWorkDue(state, state.retryAtMs, true),
              "approval prompt should suspend reconnect work");
  expect_true(wifiReconnectWorkDue(state, state.retryAtMs, false),
              "reconnect work should resume after approval");
  state = wifiRetryConnecting(state);
  expect_true(state.phase == WIFI_CONNECTING, "retry should enter connecting");
  state = wifiForget(state);
  expect_true(state.phase == WIFI_UNPROVISIONED && !state.provisioned,
              "forget should clear provisioned state");

  state = wifiPhysicalStart(wifiInitialState(true), 100);
  state = wifiProvisioningStartFailed(state);
  expect_true(state.phase == WIFI_CONNECTING && state.provisioned &&
              state.provisioningDeadlineMs == 0,
              "hotspot failure should fully reset provisioning and restore saved STA");
  state = wifiPhysicalStart(wifiInitialState(false), 100);
  state = wifiProvisioningStartFailed(state);
  expect_true(state.phase == WIFI_UNPROVISIONED && !state.provisioned &&
              state.provisioningDeadlineMs == 0,
              "hotspot failure without saved Wi-Fi should return to Wi-Fi off state");

  expect_true(wifiRetryDelayMs(0, 0) >= 1000 && wifiRetryDelayMs(0, 0) <= 1250,
              "first retry should be near one second");
  expect_true(wifiRetryDelayMs(10, 999) <= 60000,
              "retry delay should stay capped at one minute");

  expect_true(wifiSsidValid("Home"), "normal SSID should be valid");
  expect_true(!wifiSsidValid(""), "empty SSID should be rejected");
  expect_true(!wifiSsidValid("bad\nssid"), "control characters in SSID should be rejected");
  char longSsid[34]; memset(longSsid, 's', 33); longSsid[33] = 0;
  expect_true(!wifiSsidValid(longSsid), "SSID over 32 bytes should be rejected");
  expect_true(wifiPasswordValid(""), "open networks should be supported");
  expect_true(wifiPasswordValid("12345678"), "WPA2 minimum password should be valid");
  expect_true(!wifiPasswordValid("short"), "short WPA password should be rejected");
  expect_true(!wifiPasswordValid("1234567\n"),
              "WPA passphrases should contain printable ASCII only");
  char longPass[65]; memset(longPass, 'p', 64); longPass[64] = 0;
  expect_true(!wifiPasswordValid(longPass), "password over 63 bytes should be rejected");

  char escaped[64];
  wifiHtmlEscape("A&B<\"'>", escaped, sizeof(escaped));
  expect_true(strcmp(escaped, "A&amp;B&lt;&quot;&#39;&gt;") == 0,
              "portal SSIDs must be HTML escaped");
  char clipped[10];
  wifiStatusClip("123\n4567890", clipped, sizeof(clipped));
  expect_true(strcmp(clipped, "123?45678") == 0,
              "status text should be clipped and control-safe");

  expect_true(wifiTrustedHostTimeFresh(1000, 2000, 1500),
              "recent host UTC should be trusted");
  expect_true(!wifiTrustedHostTimeFresh(1000, 3000, 1500),
              "stale host UTC should allow SNTP fallback");
  expect_true(!wifiTrustedHostTimeFresh(0, 1000, 1500),
              "missing host UTC should not be trusted");
  return 0;
}
