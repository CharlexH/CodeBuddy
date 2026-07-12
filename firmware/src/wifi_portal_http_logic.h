#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "secure_zero.h"

static constexpr size_t WIFI_HTTP_POLL_BYTE_CAP = 64;
static constexpr size_t WIFI_HTTP_MAX_REQUEST_LINE = 128;
static constexpr size_t WIFI_HTTP_MAX_HEADER_LINE = 192;
static constexpr size_t WIFI_HTTP_MAX_HEADER_BYTES = 1024;
static constexpr uint8_t WIFI_HTTP_MAX_HEADERS = 16;
static constexpr size_t WIFI_HTTP_MAX_BODY = 512;
static constexpr uint32_t WIFI_HTTP_IDLE_TIMEOUT_MS = 1000;
static constexpr uint32_t WIFI_HTTP_TOTAL_TIMEOUT_MS = 5000;

enum WifiHttpPhase : uint8_t {
  WIFI_HTTP_REQUEST_LINE,
  WIFI_HTTP_HEADERS,
  WIFI_HTTP_BODY,
  WIFI_HTTP_COMPLETE,
  WIFI_HTTP_REJECTED,
};

enum WifiHttpMethod : uint8_t { WIFI_HTTP_NONE, WIFI_HTTP_GET, WIFI_HTTP_POST };

struct WifiHttpParser {
  WifiHttpPhase phase;
  WifiHttpMethod method;
  uint16_t statusCode;
  uint8_t headerCount;
  uint16_t headerBytes;
  uint16_t contentLength;
  uint16_t bodyLength;
  uint16_t lineLength;
  bool contentTypeValid;
  bool transferEncodingSeen;
  uint32_t startedAtMs;
  uint32_t lastByteAtMs;
  char path[64];
  char line[WIFI_HTTP_MAX_HEADER_LINE + 1];
  char body[WIFI_HTTP_MAX_BODY + 1];
};

struct WifiPortalForm {
  char nonce[17];
  char ssid[33];
  char password[64];
};

inline void wifiHttpParserReset(WifiHttpParser* parser, uint32_t now) {
  if (!parser) return;
  wifiSecureZero(parser, sizeof(*parser));
  parser->phase = WIFI_HTTP_REQUEST_LINE;
  parser->startedAtMs = parser->lastByteAtMs = now;
}

inline bool wifiHttpEqualsIgnoreCase(const char* a, const char* b) {
  while (*a && *b) {
    char ca = *a >= 'A' && *a <= 'Z' ? *a + ('a' - 'A') : *a;
    char cb = *b >= 'A' && *b <= 'Z' ? *b + ('a' - 'A') : *b;
    if (ca != cb) return false;
    ++a; ++b;
  }
  return *a == 0 && *b == 0;
}

inline void wifiHttpReject(WifiHttpParser* parser, uint16_t status) {
  parser->phase = WIFI_HTTP_REJECTED;
  parser->statusCode = status;
}

inline bool wifiHttpParseRequestLine(WifiHttpParser* parser) {
  char* firstSpace = strchr(parser->line, ' ');
  if (!firstSpace) return false;
  *firstSpace = 0;
  char* secondSpace = strchr(firstSpace + 1, ' ');
  if (!secondSpace) return false;
  *secondSpace = 0;
  const char* version = secondSpace + 1;
  if (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0) return false;
  parser->method = strcmp(parser->line, "GET") == 0 ? WIFI_HTTP_GET
      : strcmp(parser->line, "POST") == 0 ? WIFI_HTTP_POST : WIFI_HTTP_NONE;
  if (parser->method == WIFI_HTTP_NONE || strlen(firstSpace + 1) >= sizeof(parser->path)) return false;
  strcpy(parser->path, firstSpace + 1);
  return true;
}

inline bool wifiHttpParseUnsigned(const char* value, uint16_t* out) {
  if (!value || !*value || !out) return false;
  uint32_t n = 0;
  while (*value) {
    if (*value < '0' || *value > '9') return false;
    n = n * 10 + (uint8_t)(*value - '0');
    if (n > 65535) return false;
    ++value;
  }
  *out = (uint16_t)n;
  return true;
}

inline bool wifiHttpParseHeaderLine(WifiHttpParser* parser) {
  char* colon = strchr(parser->line, ':');
  if (!colon) return false;
  *colon = 0;
  char* value = colon + 1;
  while (*value == ' ' || *value == '\t') ++value;
  if (wifiHttpEqualsIgnoreCase(parser->line, "Content-Length")) {
    return wifiHttpParseUnsigned(value, &parser->contentLength);
  }
  if (wifiHttpEqualsIgnoreCase(parser->line, "Content-Type")) {
    parser->contentTypeValid = wifiHttpEqualsIgnoreCase(
      value, "application/x-www-form-urlencoded"
    );
    return parser->contentTypeValid;
  }
  if (wifiHttpEqualsIgnoreCase(parser->line, "Transfer-Encoding")) {
    parser->transferEncodingSeen = true;
    return false;
  }
  return true;
}

inline void wifiHttpFinishHeaders(WifiHttpParser* parser) {
  if (parser->method == WIFI_HTTP_GET) {
    parser->phase = WIFI_HTTP_COMPLETE;
    return;
  }
  if (parser->method != WIFI_HTTP_POST || strcmp(parser->path, "/save") != 0) {
    wifiHttpReject(parser, 405); return;
  }
  if (parser->transferEncodingSeen || !parser->contentTypeValid) {
    wifiHttpReject(parser, 415); return;
  }
  if (parser->contentLength > WIFI_HTTP_MAX_BODY) {
    wifiHttpReject(parser, 413); return;
  }
  if (parser->contentLength == 0) {
    wifiHttpReject(parser, 400); return;
  }
  parser->phase = WIFI_HTTP_BODY;
}

inline void wifiHttpConsumeLine(WifiHttpParser* parser) {
  parser->line[parser->lineLength] = 0;
  if (parser->phase == WIFI_HTTP_REQUEST_LINE) {
    if (!wifiHttpParseRequestLine(parser)) wifiHttpReject(parser, 400);
    else parser->phase = WIFI_HTTP_HEADERS;
  } else if (parser->lineLength == 0) {
    wifiHttpFinishHeaders(parser);
  } else {
    if (++parser->headerCount > WIFI_HTTP_MAX_HEADERS ||
        !wifiHttpParseHeaderLine(parser)) wifiHttpReject(parser, 400);
  }
  parser->lineLength = 0;
}

inline size_t wifiHttpParserFeed(
  WifiHttpParser* parser,
  const uint8_t* input,
  size_t inputSize,
  size_t workCap,
  uint32_t now
) {
  if (!parser || !input) return 0;
  size_t limit = inputSize < workCap ? inputSize : workCap;
  size_t consumed = 0;
  while (consumed < limit && parser->phase != WIFI_HTTP_COMPLETE &&
         parser->phase != WIFI_HTTP_REJECTED) {
    char c = (char)input[consumed++];
    parser->lastByteAtMs = now;
    if (parser->phase == WIFI_HTTP_BODY) {
      if (parser->bodyLength >= WIFI_HTTP_MAX_BODY) { wifiHttpReject(parser, 413); break; }
      parser->body[parser->bodyLength++] = c;
      if (parser->bodyLength == parser->contentLength) {
        parser->body[parser->bodyLength] = 0;
        parser->phase = WIFI_HTTP_COMPLETE;
      }
      continue;
    }
    if (++parser->headerBytes > WIFI_HTTP_MAX_HEADER_BYTES) {
      wifiHttpReject(parser, 431); break;
    }
    if (c == '\r') continue;
    if (c == '\n') { wifiHttpConsumeLine(parser); continue; }
    size_t lineMax = parser->phase == WIFI_HTTP_REQUEST_LINE
        ? WIFI_HTTP_MAX_REQUEST_LINE : WIFI_HTTP_MAX_HEADER_LINE;
    if (parser->lineLength >= lineMax) { wifiHttpReject(parser, 431); break; }
    parser->line[parser->lineLength++] = c;
  }
  return consumed;
}

inline constexpr bool wifiHttpParserTimedOut(const WifiHttpParser& parser, uint32_t now) {
  return parser.phase != WIFI_HTTP_COMPLETE && parser.phase != WIFI_HTTP_REJECTED &&
    ((uint32_t)(now - parser.lastByteAtMs) > WIFI_HTTP_IDLE_TIMEOUT_MS ||
     (uint32_t)(now - parser.startedAtMs) > WIFI_HTTP_TOTAL_TIMEOUT_MS);
}

inline int wifiHttpHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline bool wifiHttpUrlDecode(const char* input, size_t len, char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  size_t used = 0;
  for (size_t i = 0; i < len; ++i) {
    char c = input[i];
    if (c == '+') c = ' ';
    else if (c == '%') {
      if (i + 2 >= len) return false;
      int hi = wifiHttpHex(input[++i]); int lo = wifiHttpHex(input[++i]);
      if (hi < 0 || lo < 0) return false;
      c = (char)((hi << 4) | lo);
    }
    if (c == 0 || used + 1 >= outSize) return false;
    out[used++] = c;
  }
  out[used] = 0;
  return true;
}

inline bool wifiHttpDecodeForm(const char* body, size_t length, WifiPortalForm* out) {
  if (!body || !out || length > WIFI_HTTP_MAX_BODY) return false;
  wifiSecureZero(out, sizeof(*out));
  bool nonceSeen = false, ssidSeen = false, passwordSeen = false;
  size_t pos = 0;
  while (pos < length) {
    size_t end = pos; while (end < length && body[end] != '&') ++end;
    size_t equals = pos; while (equals < end && body[equals] != '=') ++equals;
    if (equals == end) { wifiSecureZero(out, sizeof(*out)); return false; }
    char key[17] = {};
    if (!wifiHttpUrlDecode(body + pos, equals - pos, key, sizeof(key))) {
      wifiSecureZero(key, sizeof(key)); wifiSecureZero(out, sizeof(*out)); return false;
    }
    const char* value = body + equals + 1; size_t valueLen = end - equals - 1;
    bool ok = false;
    if (strcmp(key, "nonce") == 0 && !nonceSeen) {
      ok = wifiHttpUrlDecode(value, valueLen, out->nonce, sizeof(out->nonce)); nonceSeen = ok;
    } else if (strcmp(key, "ssid") == 0 && !ssidSeen) {
      ok = wifiHttpUrlDecode(value, valueLen, out->ssid, sizeof(out->ssid)); ssidSeen = ok;
    } else if (strcmp(key, "password") == 0 && !passwordSeen) {
      ok = wifiHttpUrlDecode(value, valueLen, out->password, sizeof(out->password)); passwordSeen = ok;
    }
    wifiSecureZero(key, sizeof(key));
    if (!ok) { wifiSecureZero(out, sizeof(*out)); return false; }
    pos = end + (end < length ? 1 : 0);
  }
  bool complete = nonceSeen && ssidSeen && passwordSeen;
  if (!complete) wifiSecureZero(out, sizeof(*out));
  return complete;
}
