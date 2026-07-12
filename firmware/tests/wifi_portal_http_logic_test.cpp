#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wifi_portal_http_logic.h"

static void expect_true(bool condition, const char* message) {
  if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}

int main() {
  WifiHttpParser parser;
  wifiHttpParserReset(&parser, 100);
  const char* oversized =
    "POST /save HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 513\r\n\r\nsecret=must-not-be-read";
  size_t headerEnd = strstr(oversized, "\r\n\r\n") - oversized + 4;
  size_t consumed = wifiHttpParserFeed(
    &parser, (const uint8_t*)oversized, strlen(oversized), 1024, 110
  );
  expect_true(parser.phase == WIFI_HTTP_REJECTED && parser.statusCode == 413,
              "body over 512 bytes must be rejected from headers");
  expect_true(consumed == headerEnd && parser.bodyLength == 0,
              "oversized body must not be read or allocated");

  wifiHttpParserReset(&parser, 0);
  char longInput[201]; memset(longInput, 'X', 200); longInput[200] = 0;
  consumed = wifiHttpParserFeed(&parser, (const uint8_t*)longInput, 200,
                                WIFI_HTTP_POLL_BYTE_CAP, 1);
  expect_true(consumed == WIFI_HTTP_POLL_BYTE_CAP,
              "one poll must process only the fixed byte budget");

  wifiHttpParserReset(&parser, 1000);
  const char* partial = "POST /save HTTP/1.1\r\nContent-Length: 20\r\n";
  wifiHttpParserFeed(&parser, (const uint8_t*)partial, strlen(partial), 64, 1100);
  expect_true(wifiHttpParserTimedOut(parser, 1100 + WIFI_HTTP_IDLE_TIMEOUT_MS + 1),
              "slow request should hit idle deadline");

  wifiHttpParserReset(&parser, 0);
  const char* valid =
    "POST /save HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
    "Content-Length: 55\r\n\r\nnonce=0123456789abcdef&ssid=Home+WiFi&password=12345678";
  wifiHttpParserFeed(&parser, (const uint8_t*)valid, strlen(valid), 1024, 10);
  expect_true(parser.phase == WIFI_HTTP_COMPLETE, "bounded valid POST should complete");
  WifiPortalForm form = {};
  expect_true(wifiHttpDecodeForm(parser.body, parser.bodyLength, &form),
              "urlencoded body should decode into fixed buffers");
  expect_true(strcmp(form.ssid, "Home WiFi") == 0 &&
              strcmp(form.password, "12345678") == 0,
              "form fields should decode without dynamic allocation");
  wifiSecureZero(&form, sizeof(form));
  const uint8_t* bytes = (const uint8_t*)&form;
  for (size_t i = 0; i < sizeof(form); ++i) {
    expect_true(bytes[i] == 0, "zeroizer must overwrite the complete form buffer");
  }

  memset(&form, 0xa5, sizeof(form));
  const char* invalid = "nonce=0123456789abcdef&password=secretsecret";
  expect_true(!wifiHttpDecodeForm(invalid, strlen(invalid), &form),
              "missing SSID should reject the form");
  bytes = (const uint8_t*)&form;
  for (size_t i = 0; i < sizeof(form); ++i) {
    expect_true(bytes[i] == 0, "rejected form must clear every temporary field");
  }

  memset(parser.body, 0xa5, sizeof(parser.body));
  wifiHttpParserReset(&parser, 500);
  for (size_t i = 0; i < sizeof(parser.body); ++i) {
    expect_true(parser.body[i] == 0, "cancel/reset must clear the full request body");
  }
  return 0;
}
