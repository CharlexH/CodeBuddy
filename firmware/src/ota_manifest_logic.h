#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr size_t OTA_MANIFEST_MAX_BYTES = 1024;
constexpr size_t OTA_VERSION_MAX_BYTES = 64;
constexpr size_t OTA_URL_MAX_BYTES = 256;
constexpr size_t OTA_SHA256_HEX_BYTES = 64;
constexpr size_t OTA_TOKEN_MAX_BYTES = 128;
constexpr size_t OTA_SIGNATURE_MAX_BYTES = 80;
constexpr uint32_t OTA_SLOT_CAPACITY_BYTES = 0x330000UL;
constexpr uint32_t OTA_RECEIVE_WINDOW_MS = 120000UL;
constexpr uint32_t OTA_OFFER_TTL_MS = 60000UL;
constexpr uint8_t OTA_MIN_BATTERY_PERCENT = 50;

enum OtaManifestResult : uint8_t {
  OTA_MANIFEST_OK = 0,
  OTA_MANIFEST_ARGUMENT_INVALID,
  OTA_MANIFEST_TOO_LARGE,
  OTA_MANIFEST_SIGNATURE_INVALID,
  OTA_MANIFEST_NON_CANONICAL,
  OTA_MANIFEST_FIELD_INVALID,
  OTA_MANIFEST_URL_INVALID,
  OTA_MANIFEST_VERSION_INVALID,
  OTA_MANIFEST_NOT_NEWER,
};

enum OtaUrlResource : uint8_t {
  OTA_URL_MANIFEST = 0,
  OTA_URL_SIGNATURE,
  OTA_URL_FIRMWARE,
};

struct OtaManifestDescriptor {
  char version[OTA_VERSION_MAX_BYTES];
  uint32_t sizeBytes;
  char sha256[OTA_SHA256_HEX_BYTES + 1];
  char artifactUrl[OTA_URL_MAX_BYTES];
};

struct OtaOfferState {
  bool windowOpen;
  bool pending;
  uint32_t windowDeadlineMs;
  uint32_t offerDeadlineMs;
  uint32_t sizeBytes;
  char version[OTA_VERSION_MAX_BYTES];
  char manifestUrl[OTA_URL_MAX_BYTES];
  char signatureUrl[OTA_URL_MAX_BYTES];
  uint32_t generation;
  char nonce[49];
};

struct OtaUrlParts {
  uint8_t octets[4];
  uint16_t port;
  char token[OTA_TOKEN_MAX_BYTES];
  OtaUrlResource resource;
};

using OtaManifestSignatureVerifier = bool (*)(
  const uint8_t*, size_t, const uint8_t*, size_t, void*
);

inline bool otaAsciiDigit(char value) { return value >= '0' && value <= '9'; }
inline bool otaAsciiAlpha(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}
inline bool otaSemverChar(char value) {
  return otaAsciiDigit(value) || otaAsciiAlpha(value) || value == '-';
}
inline bool otaTokenChar(char value) {
  return otaSemverChar(value) || value == '_';
}
inline bool otaLowerHex(char value) {
  return otaAsciiDigit(value) || (value >= 'a' && value <= 'f');
}

inline bool otaParseCanonicalUint(
  const char* start,
  size_t length,
  uint32_t maximum,
  uint32_t* value
) {
  if (!start || !value || length == 0 || (length > 1 && start[0] == '0')) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < length; ++i) {
    if (!otaAsciiDigit(start[i])) return false;
    uint8_t digit = static_cast<uint8_t>(start[i] - '0');
    if (parsed > (maximum - digit) / 10U) return false;
    parsed = parsed * 10U + digit;
  }
  *value = parsed;
  return true;
}

inline bool otaPrivateIpv4(const uint8_t octets[4]) {
  return octets[0] == 10 ||
    (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
    (octets[0] == 192 && octets[1] == 168);
}

inline const char* otaResourceName(OtaUrlResource resource) {
  switch (resource) {
    case OTA_URL_MANIFEST: return "manifest.json";
    case OTA_URL_SIGNATURE: return "manifest.sig";
    case OTA_URL_FIRMWARE: return "firmware.bin";
  }
  return "";
}

inline bool otaParseLocalHttpsUrl(
  const char* url,
  OtaUrlResource expectedResource,
  OtaUrlParts* output
) {
  if (!url || !output) return false;
  size_t length = strnlen(url, OTA_URL_MAX_BYTES);
  if (length == 0 || length >= OTA_URL_MAX_BYTES) return false;
  constexpr char prefix[] = "https://";
  if (length <= sizeof(prefix) - 1 ||
      memcmp(url, prefix, sizeof(prefix) - 1) != 0) return false;
  size_t position = sizeof(prefix) - 1;
  OtaUrlParts parsed = {};
  for (size_t index = 0; index < 4; ++index) {
    size_t start = position;
    while (position < length && otaAsciiDigit(url[position])) ++position;
    size_t digits = position - start;
    uint32_t octet = 0;
    if (digits == 0 || digits > 3 ||
        !otaParseCanonicalUint(url + start, digits, 255, &octet)) return false;
    parsed.octets[index] = static_cast<uint8_t>(octet);
    char separator = index == 3 ? ':' : '.';
    if (position >= length || url[position++] != separator) return false;
  }
  if (!otaPrivateIpv4(parsed.octets)) return false;

  size_t portStart = position;
  while (position < length && otaAsciiDigit(url[position])) ++position;
  uint32_t port = 0;
  if (position == portStart || position >= length || url[position++] != '/' ||
      !otaParseCanonicalUint(url + portStart, position - portStart - 1, 65535, &port) ||
      port == 0) return false;
  parsed.port = static_cast<uint16_t>(port);

  size_t tokenStart = position;
  while (position < length && otaTokenChar(url[position])) ++position;
  size_t tokenLength = position - tokenStart;
  if (tokenLength < 24 || tokenLength >= OTA_TOKEN_MAX_BYTES ||
      position >= length || url[position++] != '/') return false;
  memcpy(parsed.token, url + tokenStart, tokenLength);
  parsed.token[tokenLength] = 0;

  const char* resourceName = otaResourceName(expectedResource);
  size_t resourceLength = strlen(resourceName);
  if (position + resourceLength != length ||
      memcmp(url + position, resourceName, resourceLength) != 0) return false;
  parsed.resource = expectedResource;
  *output = parsed;
  return true;
}

inline bool otaLocalHttpsUrlValid(const char* url, OtaUrlResource resource) {
  OtaUrlParts ignored = {};
  return otaParseLocalHttpsUrl(url, resource, &ignored);
}

inline bool otaUrlEndpointsEqual(const OtaUrlParts& left, const OtaUrlParts& right) {
  return memcmp(left.octets, right.octets, sizeof(left.octets)) == 0 &&
    left.port == right.port && strcmp(left.token, right.token) == 0;
}

inline bool otaLocalHttpsUrlsShareEndpoint(
  const char* left,
  OtaUrlResource leftResource,
  const char* right,
  OtaUrlResource rightResource
) {
  OtaUrlParts leftParts = {}, rightParts = {};
  return otaParseLocalHttpsUrl(left, leftResource, &leftParts) &&
    otaParseLocalHttpsUrl(right, rightResource, &rightParts) &&
    otaUrlEndpointsEqual(leftParts, rightParts);
}

struct OtaSemanticVersion {
  uint32_t major;
  uint32_t minor;
  uint32_t patch;
  char prerelease[OTA_VERSION_MAX_BYTES];
};

inline bool otaValidateIdentifiers(
  const char* start,
  size_t length,
  bool rejectNumericLeadingZeros
) {
  if (length == 0) return false;
  size_t position = 0;
  while (position < length) {
    size_t identifierStart = position;
    bool numeric = true;
    while (position < length && start[position] != '.') {
      if (!otaSemverChar(start[position])) return false;
      if (!otaAsciiDigit(start[position])) numeric = false;
      ++position;
    }
    size_t identifierLength = position - identifierStart;
    if (identifierLength == 0 ||
        (rejectNumericLeadingZeros && numeric && identifierLength > 1 &&
         start[identifierStart] == '0')) return false;
    if (position < length) ++position;
    if (position == length && start[length - 1] == '.') return false;
  }
  return true;
}

inline bool otaParseSemanticVersion(const char* value, OtaSemanticVersion* output) {
  if (!value || !output) return false;
  size_t length = strnlen(value, OTA_VERSION_MAX_BYTES);
  if (length == 0 || length >= OTA_VERSION_MAX_BYTES) return false;
  OtaSemanticVersion parsed = {};
  uint32_t* core[] = {&parsed.major, &parsed.minor, &parsed.patch};
  size_t position = 0;
  for (size_t component = 0; component < 3; ++component) {
    size_t start = position;
    while (position < length && otaAsciiDigit(value[position])) ++position;
    if (position == start ||
        !otaParseCanonicalUint(value + start, position - start, UINT32_MAX, core[component]))
      return false;
    if (component < 2) {
      if (position >= length || value[position++] != '.') return false;
    }
  }

  if (position < length && value[position] == '-') {
    size_t prereleaseStart = ++position;
    while (position < length && value[position] != '+') ++position;
    size_t prereleaseLength = position - prereleaseStart;
    if (prereleaseLength >= sizeof(parsed.prerelease) ||
        !otaValidateIdentifiers(value + prereleaseStart, prereleaseLength, true)) return false;
    memcpy(parsed.prerelease, value + prereleaseStart, prereleaseLength);
    parsed.prerelease[prereleaseLength] = 0;
  }
  if (position < length && value[position] == '+') {
    size_t buildStart = ++position;
    if (!otaValidateIdentifiers(value + buildStart, length - buildStart, false)) return false;
    position = length;
  }
  if (position != length) return false;
  *output = parsed;
  return true;
}

inline int otaCompareNumericIdentifier(
  const char* left, size_t leftLength, const char* right, size_t rightLength
) {
  if (leftLength != rightLength) return leftLength > rightLength ? 1 : -1;
  int compared = memcmp(left, right, leftLength);
  return compared > 0 ? 1 : compared < 0 ? -1 : 0;
}

inline int otaComparePrerelease(const char* left, const char* right) {
  bool leftEmpty = !left[0], rightEmpty = !right[0];
  if (leftEmpty || rightEmpty) {
    if (leftEmpty && rightEmpty) return 0;
    return leftEmpty ? 1 : -1;
  }
  size_t leftPosition = 0, rightPosition = 0;
  while (true) {
    size_t leftStart = leftPosition, rightStart = rightPosition;
    bool leftNumeric = true, rightNumeric = true;
    while (left[leftPosition] && left[leftPosition] != '.') {
      if (!otaAsciiDigit(left[leftPosition])) leftNumeric = false;
      ++leftPosition;
    }
    while (right[rightPosition] && right[rightPosition] != '.') {
      if (!otaAsciiDigit(right[rightPosition])) rightNumeric = false;
      ++rightPosition;
    }
    size_t leftLength = leftPosition - leftStart, rightLength = rightPosition - rightStart;
    int compared = 0;
    if (leftNumeric && rightNumeric) {
      compared = otaCompareNumericIdentifier(
        left + leftStart, leftLength, right + rightStart, rightLength
      );
    } else if (leftNumeric != rightNumeric) {
      compared = leftNumeric ? -1 : 1;
    } else {
      size_t common = leftLength < rightLength ? leftLength : rightLength;
      int lexical = memcmp(left + leftStart, right + rightStart, common);
      compared = lexical > 0 ? 1 : lexical < 0 ? -1 :
        leftLength > rightLength ? 1 : leftLength < rightLength ? -1 : 0;
    }
    if (compared != 0) return compared;
    bool leftDone = left[leftPosition] == 0, rightDone = right[rightPosition] == 0;
    if (leftDone || rightDone) {
      if (leftDone && rightDone) return 0;
      return leftDone ? -1 : 1;
    }
    ++leftPosition;
    ++rightPosition;
  }
}

inline int otaCompareSemanticVersions(const char* left, const char* right, bool* valid) {
  OtaSemanticVersion leftVersion = {}, rightVersion = {};
  if (!otaParseSemanticVersion(left, &leftVersion) ||
      !otaParseSemanticVersion(right, &rightVersion)) {
    if (valid) *valid = false;
    return 0;
  }
  if (valid) *valid = true;
  const uint32_t leftCore[] = {leftVersion.major, leftVersion.minor, leftVersion.patch};
  const uint32_t rightCore[] = {rightVersion.major, rightVersion.minor, rightVersion.patch};
  for (size_t i = 0; i < 3; ++i) {
    if (leftCore[i] != rightCore[i]) return leftCore[i] > rightCore[i] ? 1 : -1;
  }
  return otaComparePrerelease(leftVersion.prerelease, rightVersion.prerelease);
}

inline bool otaMatchBytes(
  const uint8_t* raw,
  size_t length,
  size_t* position,
  const char* literal
) {
  size_t literalLength = strlen(literal);
  if (*position + literalLength > length ||
      memcmp(raw + *position, literal, literalLength) != 0) return false;
  *position += literalLength;
  return true;
}

inline OtaManifestResult otaManifestParseCanonical(
  const uint8_t* raw,
  size_t length,
  const char* currentVersion,
  OtaManifestDescriptor* output
) {
  if (output) memset(output, 0, sizeof(*output));
  if (!raw || !currentVersion || !output || length == 0) return OTA_MANIFEST_ARGUMENT_INVALID;
  if (length > OTA_MANIFEST_MAX_BYTES) return OTA_MANIFEST_TOO_LARGE;
  OtaManifestDescriptor parsed = {};
  size_t position = 0;
  if (!otaMatchBytes(raw, length, &position, "{\"artifact\":{\"sha256\":\""))
    return OTA_MANIFEST_NON_CANONICAL;
  if (position + OTA_SHA256_HEX_BYTES > length) return OTA_MANIFEST_NON_CANONICAL;
  for (size_t i = 0; i < OTA_SHA256_HEX_BYTES; ++i) {
    char value = static_cast<char>(raw[position + i]);
    if (!otaLowerHex(value)) return OTA_MANIFEST_FIELD_INVALID;
    parsed.sha256[i] = value;
  }
  position += OTA_SHA256_HEX_BYTES;
  parsed.sha256[OTA_SHA256_HEX_BYTES] = 0;
  if (!otaMatchBytes(raw, length, &position, "\",\"sizeBytes\":"))
    return OTA_MANIFEST_NON_CANONICAL;
  size_t sizeStart = position;
  while (position < length && otaAsciiDigit(static_cast<char>(raw[position]))) ++position;
  if (position == sizeStart || position >= length ||
      !otaParseCanonicalUint(
        reinterpret_cast<const char*>(raw + sizeStart), position - sizeStart,
        OTA_SLOT_CAPACITY_BYTES, &parsed.sizeBytes
      ) || parsed.sizeBytes == 0) return OTA_MANIFEST_FIELD_INVALID;
  if (!otaMatchBytes(raw, length, &position, ",\"url\":\""))
    return OTA_MANIFEST_NON_CANONICAL;
  size_t urlStart = position;
  while (position < length && raw[position] != '"') {
    uint8_t value = raw[position];
    if (value < 0x21 || value > 0x7e || value == '\\') return OTA_MANIFEST_NON_CANONICAL;
    ++position;
  }
  size_t urlLength = position - urlStart;
  if (urlLength == 0 || urlLength >= sizeof(parsed.artifactUrl) || position >= length)
    return OTA_MANIFEST_URL_INVALID;
  memcpy(parsed.artifactUrl, raw + urlStart, urlLength);
  parsed.artifactUrl[urlLength] = 0;
  if (!otaMatchBytes(
        raw, length, &position,
        "\"},\"chip\":\"esp32s3\",\"schema\":1,\"version\":\""
      )) return OTA_MANIFEST_NON_CANONICAL;
  size_t versionStart = position;
  while (position < length && raw[position] != '"') {
    uint8_t value = raw[position];
    if (value < 0x21 || value > 0x7e || value == '\\') return OTA_MANIFEST_NON_CANONICAL;
    ++position;
  }
  size_t versionLength = position - versionStart;
  if (versionLength == 0 || versionLength >= sizeof(parsed.version) || position >= length)
    return OTA_MANIFEST_VERSION_INVALID;
  memcpy(parsed.version, raw + versionStart, versionLength);
  parsed.version[versionLength] = 0;
  if (!otaMatchBytes(raw, length, &position, "\"}") || position != length)
    return OTA_MANIFEST_NON_CANONICAL;
  if (!otaLocalHttpsUrlValid(parsed.artifactUrl, OTA_URL_FIRMWARE))
    return OTA_MANIFEST_URL_INVALID;
  bool versionsValid = false;
  int comparison = otaCompareSemanticVersions(parsed.version, currentVersion, &versionsValid);
  if (!versionsValid) return OTA_MANIFEST_VERSION_INVALID;
  if (comparison <= 0) return OTA_MANIFEST_NOT_NEWER;
  *output = parsed;
  return OTA_MANIFEST_OK;
}

inline OtaManifestResult otaManifestAuthenticateWith(
  const uint8_t* raw,
  size_t rawLength,
  const uint8_t* signature,
  size_t signatureLength,
  const char* currentVersion,
  OtaManifestSignatureVerifier verifier,
  void* verifierContext,
  OtaManifestDescriptor* output
) {
  if (output) memset(output, 0, sizeof(*output));
  if (!raw || !signature || !currentVersion || !verifier || !output ||
      rawLength == 0 || signatureLength == 0) return OTA_MANIFEST_ARGUMENT_INVALID;
  if (rawLength > OTA_MANIFEST_MAX_BYTES) return OTA_MANIFEST_TOO_LARGE;
  if (signatureLength > OTA_SIGNATURE_MAX_BYTES ||
      !verifier(raw, rawLength, signature, signatureLength, verifierContext))
    return OTA_MANIFEST_SIGNATURE_INVALID;
  return otaManifestParseCanonical(raw, rawLength, currentVersion, output);
}

inline OtaOfferState otaOfferStateInitial() {
  OtaOfferState state = {};
  return state;
}

inline void otaOfferReset(OtaOfferState* offer) {
  if (offer) memset(offer, 0, sizeof(*offer));
}

inline bool otaOfferDeadlineActive(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) < 0;
}

inline bool otaOfferOpenReceiveWindow(
  OtaOfferState* offer,
  uint32_t now,
  bool physicalAction
) {
  if (!offer || !physicalAction) return false;
  otaOfferReset(offer);
  offer->windowOpen = true;
  offer->windowDeadlineMs = now + OTA_RECEIVE_WINDOW_MS;
  return true;
}

inline void otaOfferCancel(OtaOfferState* offer) { otaOfferReset(offer); }
inline void otaOfferReject(OtaOfferState* offer) { otaOfferReset(offer); }

inline bool otaOfferConsume(OtaOfferState* source, OtaOfferState* destination) {
  if (!source || !destination || source == destination || !source->pending) {
    if (destination && destination != source) otaOfferReset(destination);
    return false;
  }
  *destination = *source;
  otaOfferReset(source);
  return true;
}

inline bool otaOfferWindowActive(const OtaOfferState& offer, uint32_t now) {
  return offer.windowOpen && otaOfferDeadlineActive(now, offer.windowDeadlineMs);
}

inline bool otaOfferAcceptHint(
  const char* version,
  uint32_t sizeBytes,
  const char* manifestUrl,
  const char* signatureUrl,
  const char* currentVersion,
  uint32_t now,
  bool connected,
  bool approvalConflict,
  bool transferConflict,
  bool otherConflict,
  bool externalPower,
  uint8_t batteryPercent,
  OtaOfferState* output
) {
  bool windowActive = output && otaOfferWindowActive(*output, now);
  if (!output || !version || !manifestUrl || !signatureUrl || !currentVersion ||
      !windowActive || !connected || approvalConflict || transferConflict ||
      otherConflict || (!externalPower && batteryPercent < OTA_MIN_BATTERY_PERCENT) ||
      batteryPercent > 100 || sizeBytes == 0 ||
      sizeBytes > OTA_SLOT_CAPACITY_BYTES) {
    otaOfferReset(output);
    return false;
  }
  bool versionsValid = false;
  if (otaCompareSemanticVersions(version, currentVersion, &versionsValid) <= 0 ||
      !versionsValid) {
    otaOfferReset(output);
    return false;
  }
  if (!otaLocalHttpsUrlsShareEndpoint(
        manifestUrl, OTA_URL_MANIFEST, signatureUrl, OTA_URL_SIGNATURE
      )) {
    otaOfferReset(output);
    return false;
  }
  size_t manifestLength = strnlen(manifestUrl, sizeof(output->manifestUrl));
  size_t signatureLength = strnlen(signatureUrl, sizeof(output->signatureUrl));
  size_t versionLength = strnlen(version, sizeof(output->version));
  if (versionLength >= sizeof(output->version) ||
      manifestLength >= sizeof(output->manifestUrl) ||
      signatureLength >= sizeof(output->signatureUrl)) {
    otaOfferReset(output);
    return false;
  }
  memcpy(output->manifestUrl, manifestUrl, manifestLength + 1);
  memcpy(output->signatureUrl, signatureUrl, signatureLength + 1);
  memcpy(output->version, version, versionLength + 1);
  output->sizeBytes = sizeBytes;
  output->pending = true;
  output->offerDeadlineMs = now + OTA_OFFER_TTL_MS;
  return true;
}

inline bool otaOfferAcceptBoundHint(
  const char* nonce,
  uint32_t generation,
  const char* version,
  uint32_t sizeBytes,
  const char* manifestUrl,
  const char* signatureUrl,
  const char* currentVersion,
  uint32_t now,
  bool connected,
  bool approvalConflict,
  bool transferConflict,
  bool otherConflict,
  bool externalPower,
  uint8_t batteryPercent,
  OtaOfferState* output
) {
  if (!nonce || !generation) {
    otaOfferReset(output);
    return false;
  }
  size_t nonceLength = strnlen(nonce, 49);
  if (nonceLength < 24 || nonceLength > 48) {
    otaOfferReset(output);
    return false;
  }
  for (size_t i = 0; i < nonceLength; ++i) {
    if (!otaTokenChar(nonce[i])) {
      otaOfferReset(output);
      return false;
    }
  }
  if (!otaOfferAcceptHint(
        version, sizeBytes, manifestUrl, signatureUrl, currentVersion, now,
        connected, approvalConflict, transferConflict, otherConflict,
        externalPower, batteryPercent, output
      )) return false;
  output->generation = generation;
  memcpy(output->nonce, nonce, nonceLength + 1);
  return true;
}

inline void otaOfferLifecyclePoll(
  OtaOfferState* offer,
  uint32_t now,
  bool connected,
  bool approvalConflict,
  bool transferConflict,
  bool otherConflict
) {
  if (!offer || !offer->windowOpen) return;
  if (!connected || approvalConflict || transferConflict || otherConflict ||
      !otaOfferWindowActive(*offer, now) ||
      (offer->pending && !otaOfferDeadlineActive(now, offer->offerDeadlineMs)))
    otaOfferReset(offer);
}

inline bool otaOfferExecutionAllowed(
  OtaOfferState* offer,
  uint32_t now,
  bool connected,
  bool approvalConflict,
  bool transferConflict,
  bool otherConflict,
  bool externalPower,
  uint8_t batteryPercent
) {
  bool allowed = offer && offer->pending &&
    otaOfferWindowActive(*offer, now) &&
    otaOfferDeadlineActive(now, offer->offerDeadlineMs) && connected &&
    !approvalConflict && !transferConflict && !otherConflict &&
    (externalPower || (batteryPercent >= OTA_MIN_BATTERY_PERCENT &&
                       batteryPercent <= 100));
  if (!allowed) otaOfferReset(offer);
  return allowed;
}

inline bool otaManifestMatchesOffer(
  const OtaManifestDescriptor& manifest,
  const OtaOfferState& offer
) {
  return offer.pending && manifest.sizeBytes == offer.sizeBytes &&
    strcmp(manifest.version, offer.version) == 0 &&
    otaLocalHttpsUrlsShareEndpoint(
      manifest.artifactUrl, OTA_URL_FIRMWARE, offer.manifestUrl, OTA_URL_MANIFEST
    ) &&
    otaLocalHttpsUrlsShareEndpoint(
      manifest.artifactUrl, OTA_URL_FIRMWARE, offer.signatureUrl, OTA_URL_SIGNATURE
    );
}
