#include "ota_update.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_tls.h>
#include <esp_tls_errors.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>
#include <stdio.h>

#include "ota_manifest.h"
#include "ota_policy_logic.h"
#include "ota_trust.h"

namespace {

enum OtaHttpRequestPhase : uint8_t {
  OTA_HTTP_REQUEST_IDLE = 0,
  OTA_HTTP_REQUEST_CONNECT,
  OTA_HTTP_REQUEST_WRITE,
  OTA_HTTP_REQUEST_HEADER,
  OTA_HTTP_REQUEST_BODY,
  OTA_HTTP_REQUEST_EXPECT_EOF,
  OTA_HTTP_REQUEST_EOF,
};

struct OtaHttpRequest {
  esp_tls_t* tls;
  esp_tls_cfg_t config;
  OtaHttpBodyState body;
  OtaHttpRequestPhase phase;
  OtaUrlResource resource;
  uint32_t maximumLength;
  uint32_t exactLength;
  uint32_t absoluteTimeoutMs;
  uint32_t startedAtMs;
  uint32_t openDeadlineMs;
  uint32_t idleDeadlineMs;
  size_t requestLength;
  size_t requestWritten;
  size_t headerLength;
  size_t prefetchedOffset;
  size_t prefetchedEnd;
  uint32_t pendingFinalBytes;
  char url[OTA_URL_MAX_BYTES];
  char outbound[OTA_URL_MAX_BYTES + 96];
  uint8_t header[OTA_UPDATE_HTTP_HEADER_MAX_BYTES];
  bool open;
};

struct OtaRuntime {
  OtaUpdateMachine machine;
  OtaOfferState* sourceOffer;
  OtaOfferState offer;
  bool offerConsumed;
  OtaUpdateRuntimeInputs inputs;
  OtaManifestDescriptor descriptor;
  OtaDisplayMetadata display;
  uint8_t manifest[OTA_MANIFEST_MAX_BYTES];
  size_t manifestLength;
  uint8_t signature[OTA_SIGNATURE_MAX_BYTES];
  size_t signatureLength;
  uint8_t buffer[OTA_UPDATE_CHUNK_BYTES];
  uint8_t expectedDigest[32];
  uint8_t readbackDigest[32];
  mbedtls_sha256_context readbackHash;
  bool readbackHashReady;
  const esp_partition_t* running;
  const esp_partition_t* target;
  esp_ota_handle_t handle;
  OtaHttpRequest request;
  bool confirmRequested;
  bool cancelRequested;
  bool automatic;
  bool active;
  uint32_t terminalSinceMs;
  char status[24];
};

OtaRuntime runtime = {};

void setStatus(const char* value) {
  strncpy(runtime.status, value ? value : "", sizeof(runtime.status) - 1);
  runtime.status[sizeof(runtime.status) - 1] = 0;
}

void closeRequest() {
  if (runtime.request.tls) esp_tls_conn_destroy(runtime.request.tls);
  mbedtls_platform_zeroize(&runtime.request, sizeof(runtime.request));
}

void reconstructRequest() {
  closeRequest();
}

bool tlsWouldBlock(ssize_t result) {
  return result == ESP_TLS_ERR_SSL_WANT_READ ||
    result == ESP_TLS_ERR_SSL_WANT_WRITE;
}

size_t findHeaderEnd(const uint8_t* value, size_t length) {
  if (!value || length < 4) return 0;
  for (size_t index = 0; index + 3 < length; ++index) {
    if (value[index] == '\r' && value[index + 1] == '\n' &&
        value[index + 2] == '\r' && value[index + 3] == '\n')
      return index + 4;
  }
  return 0;
}

bool startRequest(
  const char* url,
  OtaUrlResource resource,
  uint32_t maximumLength,
  uint32_t exactLength,
  uint32_t absoluteTimeoutMs
) {
  if (!url || !url[0] || !maximumLength || !absoluteTimeoutMs ||
      !otaTrustLocalCaPem() || !otaTrustLocalCaPem()[0])
    return false;
  closeRequest();
  OtaUrlParts parts = {};
  if (!otaParseLocalHttpsUrl(url, resource, &parts)) return false;
  size_t urlLength = strnlen(url, sizeof(runtime.request.url));
  if (!urlLength || urlLength >= sizeof(runtime.request.url)) return false;
  memcpy(runtime.request.url, url, urlLength + 1);
  int requestLength = snprintf(
    runtime.request.outbound,
    sizeof(runtime.request.outbound),
    "GET /%s/%s HTTP/1.1\r\n"
    "Host: %u.%u.%u.%u:%u\r\n"
    "Connection: close\r\n"
    "Accept-Encoding: identity\r\n\r\n",
    parts.token,
    otaResourceName(resource),
    parts.octets[0], parts.octets[1], parts.octets[2], parts.octets[3],
    parts.port
  );
  if (requestLength <= 0 ||
      static_cast<size_t>(requestLength) >= sizeof(runtime.request.outbound)) {
    closeRequest();
    return false;
  }
  runtime.request.tls = esp_tls_init();
  if (!runtime.request.tls) {
    closeRequest();
    return false;
  }
  runtime.request.config = {};
  runtime.request.config.cacert_buf = reinterpret_cast<const unsigned char*>(
    otaTrustLocalCaPem()
  );
  runtime.request.config.cacert_bytes = strlen(otaTrustLocalCaPem()) + 1;
  runtime.request.config.non_block = true;
  // IDF 4.4 passes a null timeval (and blocks indefinitely) when this is 0.
  // One millisecond bounds each connect readiness probe; the application-level
  // five-second deadline remains authoritative across polls.
  runtime.request.config.timeout_ms = 1;
  runtime.request.config.skip_common_name = false;
  runtime.request.phase = OTA_HTTP_REQUEST_CONNECT;
  runtime.request.resource = resource;
  runtime.request.maximumLength = maximumLength;
  runtime.request.exactLength = exactLength;
  runtime.request.absoluteTimeoutMs = absoluteTimeoutMs;
  runtime.request.startedAtMs = millis();
  runtime.request.openDeadlineMs =
    runtime.request.startedAtMs + OTA_UPDATE_OPEN_TIMEOUT_MS;
  runtime.request.idleDeadlineMs = runtime.request.openDeadlineMs;
  runtime.request.requestLength = static_cast<size_t>(requestLength);
  runtime.request.open = true;
  return true;
}

OtaUpdateActionResult pollOpenRequest(
  const char* url,
  OtaUrlResource resource,
  uint32_t maximumLength,
  uint32_t exactLength,
  uint32_t absoluteTimeoutMs,
  OtaUpdateFailure contentFailure
) {
  if (runtime.request.phase == OTA_HTTP_REQUEST_IDLE) {
    return startRequest(
      url, resource, maximumLength, exactLength, absoluteTimeoutMs
    ) ? otaActionPending() : otaActionFailure(OTA_UPDATE_FAILURE_TRUST);
  }
  if (!runtime.request.open || !runtime.request.tls ||
      runtime.request.resource != resource ||
      runtime.request.maximumLength != maximumLength ||
      runtime.request.exactLength != exactLength ||
      runtime.request.absoluteTimeoutMs != absoluteTimeoutMs ||
      strcmp(runtime.request.url, url) != 0)
    return otaActionFailure(contentFailure);
  uint32_t now = millis();
  if (otaHttpIoExpired(
        now,
        runtime.request.openDeadlineMs,
        runtime.request.idleDeadlineMs
      ))
    return otaActionFailure(OTA_UPDATE_FAILURE_TIMEOUT);

  if (runtime.request.phase == OTA_HTTP_REQUEST_CONNECT) {
    // select() mutates fd_sets. IDF 4.4 initializes these only in ESP_TLS_INIT,
    // so restore them before every later CONNECTING probe or a timed-out poll
    // can leave the async connector permanently watching empty sets.
    if (runtime.request.tls->conn_state == ESP_TLS_CONNECTING) {
      FD_ZERO(&runtime.request.tls->rset);
      FD_SET(runtime.request.tls->sockfd, &runtime.request.tls->rset);
      runtime.request.tls->wset = runtime.request.tls->rset;
    }
    int connected = esp_tls_conn_http_new_async(
      runtime.request.url,
      &runtime.request.config,
      runtime.request.tls
    );
    if (connected < 0) return otaActionFailure(OTA_UPDATE_FAILURE_TRUST);
    if (connected == 0) return otaActionPending();
    runtime.request.phase = OTA_HTTP_REQUEST_WRITE;
    runtime.request.idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
    return otaActionPending();
  }

  if (runtime.request.phase == OTA_HTTP_REQUEST_WRITE) {
    size_t remaining = runtime.request.requestLength -
      runtime.request.requestWritten;
    if (!remaining) {
      runtime.request.phase = OTA_HTTP_REQUEST_HEADER;
      return otaActionPending();
    }
    ssize_t written = esp_tls_conn_write(
      runtime.request.tls,
      runtime.request.outbound + runtime.request.requestWritten,
      remaining
    );
    if (tlsWouldBlock(written)) return otaActionPending();
    if (written <= 0 || static_cast<size_t>(written) > remaining)
      return otaActionFailure(OTA_UPDATE_FAILURE_TRUST);
    runtime.request.requestWritten += static_cast<size_t>(written);
    runtime.request.idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
    if (runtime.request.requestWritten == runtime.request.requestLength)
      runtime.request.phase = OTA_HTTP_REQUEST_HEADER;
    return otaActionPending();
  }

  if (runtime.request.phase != OTA_HTTP_REQUEST_HEADER)
    return runtime.request.phase == OTA_HTTP_REQUEST_BODY
      ? otaActionComplete() : otaActionFailure(OTA_UPDATE_FAILURE_TRUST);
  size_t capacity = sizeof(runtime.request.header) - runtime.request.headerLength;
  if (!capacity) return otaActionFailure(contentFailure);
  ssize_t received = esp_tls_conn_read(
    runtime.request.tls,
    runtime.request.header + runtime.request.headerLength,
    capacity
  );
  if (tlsWouldBlock(received)) return otaActionPending();
  if (received <= 0 || static_cast<size_t>(received) > capacity)
    return otaActionFailure(contentFailure);
  runtime.request.headerLength += static_cast<size_t>(received);
  runtime.request.idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
  size_t headerEnd = findHeaderEnd(
    runtime.request.header, runtime.request.headerLength
  );
  if (!headerEnd) return runtime.request.headerLength < sizeof(runtime.request.header)
    ? otaActionPending() : otaActionFailure(contentFailure);
  OtaHttpMeta meta = {};
  if (!otaParseStrictHttpResponseHeader(
        runtime.request.header, headerEnd, &meta
      )) return otaActionFailure(contentFailure);
  bool valid = exactLength
    ? otaHttpMetaExactValid(meta, exactLength, maximumLength)
    : otaHttpMetaBoundedValid(meta, maximumLength);
  if (!valid) return otaActionFailure(contentFailure);
  if (!otaPrefetchedBodyValid(
        runtime.request.headerLength,
        headerEnd,
        static_cast<uint32_t>(meta.contentLength)
      )) return otaActionFailure(contentFailure);
  runtime.request.body = otaHttpBodyState(
    runtime.request.startedAtMs,
    static_cast<uint32_t>(meta.contentLength),
    absoluteTimeoutMs
  );
  runtime.request.body.idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
  runtime.request.prefetchedOffset = headerEnd;
  runtime.request.prefetchedEnd = runtime.request.headerLength;
  runtime.request.phase = OTA_HTTP_REQUEST_BODY;
  return otaActionComplete();
}

OtaUpdateActionResult readRequest(
  uint8_t* output,
  size_t outputCapacity,
  uint32_t maximumBytes,
  bool append,
  OtaUpdateFailure contentFailure
) {
  if (!runtime.request.open ||
      (runtime.request.phase != OTA_HTTP_REQUEST_BODY &&
       runtime.request.phase != OTA_HTTP_REQUEST_EXPECT_EOF) ||
      !runtime.request.tls || !output || !outputCapacity || !maximumBytes)
    return otaActionFailure(contentFailure);
  uint32_t now = millis();
  if (otaHttpIoExpired(
        now,
        runtime.request.body.absoluteDeadlineMs,
        runtime.request.body.idleDeadlineMs
      ))
    return otaActionFailure(OTA_UPDATE_FAILURE_TIMEOUT);
  if (runtime.request.phase == OTA_HTTP_REQUEST_EXPECT_EOF) {
    uint8_t unexpected = 0;
    ssize_t trailing = esp_tls_conn_read(
      runtime.request.tls, &unexpected, sizeof(unexpected)
    );
    OtaHttpEofDecision eof = otaHttpEofDecision(
      static_cast<int32_t>(trailing),
      ESP_TLS_ERR_SSL_WANT_READ,
      ESP_TLS_ERR_SSL_WANT_WRITE
    );
    if (eof == OTA_HTTP_EOF_WAIT) return otaActionPending();
    if (eof != OTA_HTTP_EOF_COMPLETE || !runtime.request.pendingFinalBytes)
      return otaActionFailure(contentFailure);
    uint32_t finalBytes = runtime.request.pendingFinalBytes;
    runtime.request.pendingFinalBytes = 0;
    if (!otaHttpBodyCommit(&runtime.request.body, now, finalBytes))
      return otaActionFailure(contentFailure);
    runtime.request.phase = OTA_HTTP_REQUEST_EOF;
    return otaActionComplete(finalBytes);
  }
  if (!runtime.request.body.expectedBytes ||
      runtime.request.body.receivedBytes >= runtime.request.body.expectedBytes)
    return otaActionFailure(contentFailure);
  uint32_t remaining = runtime.request.body.expectedBytes -
    runtime.request.body.receivedBytes;
  if (!remaining) return otaActionFailure(contentFailure);
  uint32_t requested = remaining < maximumBytes ? remaining : maximumBytes;
  size_t destinationOffset = append ? runtime.request.body.receivedBytes : 0;
  if (destinationOffset > outputCapacity ||
      requested > outputCapacity - destinationOffset)
    return otaActionFailure(contentFailure);

  uint32_t actual = 0;
  size_t prefetched = runtime.request.prefetchedEnd -
    runtime.request.prefetchedOffset;
  if (prefetched) {
    if (prefetched > remaining) return otaActionFailure(contentFailure);
    actual = prefetched < requested
      ? static_cast<uint32_t>(prefetched) : requested;
    memcpy(
      output + destinationOffset,
      runtime.request.header + runtime.request.prefetchedOffset,
      actual
    );
    runtime.request.prefetchedOffset += actual;
  } else {
    ssize_t received = esp_tls_conn_read(
      runtime.request.tls,
      output + destinationOffset,
      requested
    );
    if (tlsWouldBlock(received)) return otaActionPending();
    if (received <= 0 || static_cast<uint32_t>(received) > requested)
      return otaActionFailure(contentFailure);
    actual = static_cast<uint32_t>(received);
  }
  if (actual == remaining) {
    runtime.request.pendingFinalBytes = actual;
    runtime.request.body.idleDeadlineMs = now + OTA_UPDATE_IO_IDLE_TIMEOUT_MS;
    runtime.request.phase = OTA_HTTP_REQUEST_EXPECT_EOF;
    return otaActionPending();
  }
  if (!otaHttpBodyCommit(&runtime.request.body, now, actual))
    return otaActionFailure(contentFailure);
  return otaActionProgress(actual);
}

OtaOfferState* coordinatedOffer() {
  return runtime.offerConsumed ? &runtime.offer : runtime.sourceOffer;
}

bool currentFinalGate() {
  if (!runtime.machine.authenticated || !runtime.target ||
      esp_ota_get_running_partition() != runtime.running)
    return false;
  return !runtime.inputs.prompt && !runtime.inputs.transfer &&
    !runtime.inputs.provisioning && !runtime.inputs.passkey &&
    !runtime.inputs.functional && runtime.inputs.wifiProvisioned &&
    runtime.inputs.wifiOnline && runtime.inputs.trustedTime &&
    (runtime.inputs.externalPower ||
      (runtime.inputs.batteryKnown &&
       runtime.inputs.batteryPercent >= OTA_MIN_BATTERY_PERCENT &&
       runtime.inputs.batteryPercent <= 100));
}

bool partitionSelectionValid() {
  runtime.running = esp_ota_get_running_partition();
  runtime.target = esp_ota_get_next_update_partition(runtime.running);
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_err_t stateResult = runtime.running
    ? esp_ota_get_state_partition(runtime.running, &state)
    : ESP_ERR_INVALID_ARG;
  OtaPartitionStateQuery stateQuery = OTA_STATE_QUERY_ERROR;
  if (stateResult == ESP_OK) stateQuery = OTA_STATE_QUERY_OK;
  else if (stateResult == ESP_ERR_NOT_FOUND ||
           (stateResult == ESP_ERR_NOT_SUPPORTED && runtime.running &&
            runtime.running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY))
    stateQuery = OTA_STATE_QUERY_NOT_FOUND;
  OtaRunningImageState runningState = otaRunningImageStateFromRaw(
    static_cast<uint32_t>(state)
  );
  const esp_partition_t* configuredBoot = esp_ota_get_boot_partition();
  bool runningSubtypeEligible = runtime.running &&
    (runtime.running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY ||
     (runtime.running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN &&
      runtime.running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX));
  bool initialLayoutVerified = runtime.running && configuredBoot &&
    configuredBoot == runtime.running &&
    runtime.running->type == ESP_PARTITION_TYPE_APP &&
    configuredBoot->type == ESP_PARTITION_TYPE_APP &&
    runtime.running->address == configuredBoot->address &&
    runtime.running->size == configuredBoot->size && runningSubtypeEligible;
  if (!otaRunningStateAllowsUpdate(
        stateQuery, runningState, initialLayoutVerified
      ))
    return false;
  OtaPartitionInfo running = {
    reinterpret_cast<uintptr_t>(runtime.running),
    runtime.running ? runtime.running->address : 0,
    runtime.running ? runtime.running->size : 0,
    runtime.running && runtime.running->type == ESP_PARTITION_TYPE_APP
      ? OTA_PARTITION_APP : OTA_PARTITION_DATA,
    static_cast<uint8_t>(
      runtime.running ? runtime.running->subtype : ESP_PARTITION_SUBTYPE_ANY
    ),
    runningState == OTA_RUNNING_IMAGE_PENDING_VERIFY,
  };
  OtaPartitionInfo target = {
    reinterpret_cast<uintptr_t>(runtime.target),
    runtime.target ? runtime.target->address : 0,
    runtime.target ? runtime.target->size : 0,
    runtime.target && runtime.target->type == ESP_PARTITION_TYPE_APP
      ? OTA_PARTITION_APP : OTA_PARTITION_DATA,
    static_cast<uint8_t>(
      runtime.target ? runtime.target->subtype : ESP_PARTITION_SUBTYPE_ANY
    ),
    false,
  };
  return otaTargetValid(
    runtime.running ? &running : nullptr,
    runtime.target ? &target : nullptr,
    runtime.descriptor.sizeBytes
  );
}

OtaUpdateActionResult otaAction(
  void*,
  OtaUpdateEvent event,
  uint64_t offset,
  uint32_t maximumBytes
) {
  switch (event) {
    case OTA_EVENT_OPEN_MANIFEST:
      setStatus("Checking update");
      return pollOpenRequest(
        runtime.offer.manifestUrl,
        OTA_URL_MANIFEST,
        sizeof(runtime.manifest),
        0,
        OTA_UPDATE_RESOURCE_TIMEOUT_MS,
        OTA_UPDATE_FAILURE_MANIFEST
      );
    case OTA_EVENT_READ_MANIFEST: {
      OtaUpdateActionResult result = readRequest(
        runtime.manifest,
        sizeof(runtime.manifest),
        maximumBytes,
        true,
        OTA_UPDATE_FAILURE_MANIFEST
      );
      if (result.status == OTA_ACTION_COMPLETE) {
        runtime.manifestLength = runtime.request.body.receivedBytes;
        closeRequest();
      }
      return result;
    }
    case OTA_EVENT_OPEN_SIGNATURE:
      setStatus("Checking update");
      return pollOpenRequest(
        runtime.offer.signatureUrl,
        OTA_URL_SIGNATURE,
        sizeof(runtime.signature),
        0,
        OTA_UPDATE_RESOURCE_TIMEOUT_MS,
        OTA_UPDATE_FAILURE_MANIFEST
      );
    case OTA_EVENT_READ_SIGNATURE: {
      OtaUpdateActionResult result = readRequest(
        runtime.signature,
        sizeof(runtime.signature),
        maximumBytes,
        true,
        OTA_UPDATE_FAILURE_MANIFEST
      );
      if (result.status == OTA_ACTION_COMPLETE) {
        runtime.signatureLength = runtime.request.body.receivedBytes;
        closeRequest();
      }
      return result;
    }
    case OTA_EVENT_AUTHENTICATE: {
      setStatus("Verifying update");
      if (!runtime.manifestLength || !runtime.signatureLength)
        return otaActionFailure(OTA_UPDATE_FAILURE_MANIFEST);
      OtaManifestResult manifestResult = otaManifestAuthenticateAndParse(
        runtime.manifest,
        runtime.manifestLength,
        runtime.signature,
        runtime.signatureLength,
        &runtime.descriptor
      );
      if (manifestResult != OTA_MANIFEST_OK)
        return otaActionFailure(otaUpdateFailureForManifest(manifestResult));
      if (!otaManifestMatchesOffer(runtime.descriptor, runtime.offer))
        return otaActionFailure(OTA_UPDATE_FAILURE_VERSION);
      if (!otaDecodeSha256(runtime.descriptor.sha256, runtime.expectedDigest))
        return otaActionFailure(OTA_UPDATE_FAILURE_MANIFEST);
      runtime.machine.imageSize = runtime.descriptor.sizeBytes;
      if (!otaDisplayMetadataCapture(
            &runtime.display,
            runtime.descriptor.version,
            runtime.descriptor.sizeBytes
          ))
        return otaActionFailure(OTA_UPDATE_FAILURE_VERSION);
      mbedtls_platform_zeroize(runtime.manifest, sizeof(runtime.manifest));
      mbedtls_platform_zeroize(runtime.signature, sizeof(runtime.signature));
      return otaActionComplete();
    }
    case OTA_EVENT_SELECT:
      setStatus("Preparing storage");
      return partitionSelectionValid()
        ? otaActionComplete() : otaActionFailure(OTA_UPDATE_FAILURE_ROLLBACK);
    case OTA_EVENT_OPEN_FIRMWARE:
      setStatus("Opening download");
      return pollOpenRequest(
        runtime.descriptor.artifactUrl,
        OTA_URL_FIRMWARE,
        OTA_SLOT_CAPACITY_BYTES,
        runtime.descriptor.sizeBytes,
        OTA_UPDATE_FIRMWARE_TIMEOUT_MS,
        OTA_UPDATE_FAILURE_DOWNLOAD
      );
    case OTA_EVENT_BEGIN:
      setStatus("Starting update");
      if (!currentFinalGate() || !runtime.request.open || !runtime.target ||
          maximumBytes != runtime.descriptor.sizeBytes)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      runtime.handle = 0;
      return esp_ota_begin(
        runtime.target,
        runtime.descriptor.sizeBytes,
        &runtime.handle
      ) == ESP_OK ? otaActionComplete() :
        otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
    case OTA_EVENT_DOWNLOAD:
      setStatus("Downloading");
      if (offset != runtime.request.body.receivedBytes ||
          offset >= runtime.descriptor.sizeBytes)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      return readRequest(
        runtime.buffer,
        sizeof(runtime.buffer),
        maximumBytes,
        false,
        OTA_UPDATE_FAILURE_DOWNLOAD
      );
    case OTA_EVENT_WRITE:
      if (!maximumBytes || maximumBytes > sizeof(runtime.buffer) ||
          offset + maximumBytes > runtime.descriptor.sizeBytes || !runtime.handle)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      return esp_ota_write(
        runtime.handle, runtime.buffer, maximumBytes
      ) == ESP_OK ? otaActionComplete() :
        otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
    case OTA_EVENT_FINISH_DOWNLOAD:
      if (!runtime.request.open ||
          runtime.request.phase != OTA_HTTP_REQUEST_EOF ||
          runtime.request.body.receivedBytes != runtime.descriptor.sizeBytes)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      closeRequest();
      return otaActionComplete();
    case OTA_EVENT_ABORT: {
      closeRequest();
      esp_ota_handle_t handle = runtime.handle;
      runtime.handle = 0;
      if (handle) esp_ota_abort(handle);
      return otaActionComplete();
    }
    case OTA_EVENT_END: {
      closeRequest();
      esp_ota_handle_t handle = runtime.handle;
      runtime.handle = 0;
      if (!handle || esp_ota_end(handle) != ESP_OK)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      mbedtls_sha256_init(&runtime.readbackHash);
      if (mbedtls_sha256_starts_ret(&runtime.readbackHash, 0) != 0) {
        mbedtls_sha256_free(&runtime.readbackHash);
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      }
      runtime.readbackHashReady = true;
      setStatus("Verifying storage");
      return otaActionComplete();
    }
    case OTA_EVENT_READBACK:
      if (!runtime.readbackHashReady || !maximumBytes ||
          maximumBytes > sizeof(runtime.buffer) ||
          offset + maximumBytes > runtime.descriptor.sizeBytes ||
          esp_partition_read(
            runtime.target, offset, runtime.buffer, maximumBytes
          ) != ESP_OK ||
          mbedtls_sha256_update_ret(
            &runtime.readbackHash, runtime.buffer, maximumBytes
          ) != 0)
        return otaActionFailure(OTA_UPDATE_FAILURE_DOWNLOAD);
      return otaActionComplete(maximumBytes);
    case OTA_EVENT_DIGEST_MATCH:
      if (!runtime.readbackHashReady ||
          mbedtls_sha256_finish_ret(
            &runtime.readbackHash, runtime.readbackDigest
          ) != 0)
        return otaActionFailure(OTA_UPDATE_FAILURE_HASH);
      mbedtls_sha256_free(&runtime.readbackHash);
      runtime.readbackHashReady = false;
      return otaConstantTimeEqual(
        runtime.expectedDigest, runtime.readbackDigest, 32
      ) ? otaActionComplete() : otaActionFailure(OTA_UPDATE_FAILURE_HASH);
    case OTA_EVENT_FINAL_GATE:
      setStatus("Finishing update");
      if (currentFinalGate()) return otaActionComplete();
      {
        OtaUpdateInputs inputs = {};
        inputs.wifiProvisioned = runtime.inputs.wifiProvisioned;
        inputs.wifiOnline = runtime.inputs.wifiOnline;
        inputs.trustedTime = runtime.inputs.trustedTime;
        inputs.externalPower = runtime.inputs.externalPower;
        inputs.batteryKnown = runtime.inputs.batteryKnown;
        inputs.batteryPercent = runtime.inputs.batteryPercent;
        inputs.prompt = runtime.inputs.prompt;
        inputs.transfer = runtime.inputs.transfer;
        inputs.provisioning = runtime.inputs.provisioning;
        inputs.passkey = runtime.inputs.passkey;
        inputs.functional = runtime.inputs.functional;
        return otaActionFailure(
          otaUpdateFailureForGate(otaUpdateGate(inputs, false), false)
        );
      }
    case OTA_EVENT_SET_BOOT:
      if (!runtime.target ||
          esp_ota_set_boot_partition(runtime.target) != ESP_OK)
        return otaActionFailure(OTA_UPDATE_FAILURE_ROLLBACK);
      // Latch before any cleanup work: after ESP-IDF commits otadata there is
      // no safe path back to ordinary cancellation or error handling.
      runtime.machine.bootCommitted = true;
      runtime.machine.phase = OTA_PHASE_RESTART;
      runtime.machine.restartAttempts = 0;
      runtime.machine.nextRestartAttemptMs = millis();
      setStatus("Restarting");
      otaDisplayMetadataPreserveForBootCommit(&runtime.display);
      // The boot selection is committed and must never be treated as a
      // cancellable transaction again. Scrub endpoint tokens now; restart
      // needs only the latched pure-machine state.
      reconstructRequest();
      mbedtls_platform_zeroize(&runtime.descriptor, sizeof(runtime.descriptor));
      mbedtls_platform_zeroize(runtime.buffer, sizeof(runtime.buffer));
      mbedtls_platform_zeroize(runtime.manifest, sizeof(runtime.manifest));
      mbedtls_platform_zeroize(runtime.signature, sizeof(runtime.signature));
      mbedtls_platform_zeroize(runtime.expectedDigest, sizeof(runtime.expectedDigest));
      mbedtls_platform_zeroize(runtime.readbackDigest, sizeof(runtime.readbackDigest));
      runtime.manifestLength = 0;
      runtime.signatureLength = 0;
      if (runtime.sourceOffer) otaOfferReset(runtime.sourceOffer);
      runtime.sourceOffer = nullptr;
      otaOfferReset(&runtime.offer);
      runtime.offerConsumed = false;
      runtime.running = nullptr;
      runtime.target = nullptr;
      return otaActionComplete();
    case OTA_EVENT_RESTART:
      setStatus("Restarting");
      esp_restart();
      return otaActionFailure();
    case OTA_EVENT_RESTART_FALLBACK:
      setStatus("Forcing restart");
      abort();
      return otaActionFailure();
  }
  return otaActionFailure();
}

void cleanupTerminal() {
  reconstructRequest();
  if (runtime.readbackHashReady) {
    mbedtls_sha256_free(&runtime.readbackHash);
    runtime.readbackHashReady = false;
  }
  runtime.handle = 0;
  mbedtls_platform_zeroize(runtime.buffer, sizeof(runtime.buffer));
  mbedtls_platform_zeroize(runtime.manifest, sizeof(runtime.manifest));
  mbedtls_platform_zeroize(runtime.signature, sizeof(runtime.signature));
  mbedtls_platform_zeroize(runtime.expectedDigest, sizeof(runtime.expectedDigest));
  mbedtls_platform_zeroize(runtime.readbackDigest, sizeof(runtime.readbackDigest));
  mbedtls_platform_zeroize(&runtime.descriptor, sizeof(runtime.descriptor));
  runtime.manifestLength = 0;
  runtime.signatureLength = 0;
  if (runtime.sourceOffer) otaOfferReset(runtime.sourceOffer);
  runtime.sourceOffer = nullptr;
  otaOfferReset(&runtime.offer);
  runtime.offerConsumed = false;
  runtime.running = nullptr;
  runtime.target = nullptr;
  if (runtime.machine.phase == OTA_PHASE_CANCELLED) setStatus("Update cancelled");
  else if (runtime.machine.phase == OTA_PHASE_ERROR) setStatus("Update failed");
  otaUpdateScrubMachine(&runtime.machine);
  runtime.terminalSinceMs = millis();
}

void arm(OtaOfferState* offer) {
  runtime.machine = otaUpdateMachineInitial();
  runtime.machine.action = otaAction;
  runtime.machine.actionContext = &runtime;
  runtime.machine.chunkSize = OTA_UPDATE_CHUNK_BYTES;
  runtime.sourceOffer = offer;
  otaDisplayMetadataClear(&runtime.display);
  if (offer)
    otaDisplayMetadataCapture(&runtime.display, offer->version, offer->sizeBytes);
  otaOfferReset(&runtime.offer);
  runtime.offerConsumed = false;
  runtime.active = true;
  OtaOfferPolicy policy = otaOfferPolicy(
    offer && offer->pending,
    offer && offer->signedAuthorized,
    runtime.inputs.automaticPolicy
  );
  runtime.automatic = policy.automatic;
  runtime.confirmRequested = false;
  runtime.cancelRequested = false;
  runtime.terminalSinceMs = 0;
  runtime.manifestLength = 0;
  runtime.signatureLength = 0;
  runtime.running = nullptr;
  runtime.target = nullptr;
  runtime.handle = 0;
  memset(&runtime.descriptor, 0, sizeof(runtime.descriptor));
  setStatus(runtime.automatic ? "Preparing update" : "Press A to install");
}

bool consumeOfferAfterAuthorization(bool automatic) {
  OtaOfferState* offer = runtime.sourceOffer;
  if (automatic && (!offer || !offer->signedAuthorized)) return false;
  uint8_t batteryPercent = runtime.inputs.batteryKnown
    ? runtime.inputs.batteryPercent : 0;
  bool allowed = otaOfferExecutionAllowed(
    offer,
    millis(),
    runtime.inputs.bleConnected,
    runtime.inputs.prompt,
    runtime.inputs.transfer,
    runtime.inputs.provisioning || runtime.inputs.passkey ||
      runtime.inputs.functional,
    runtime.inputs.externalPower,
    batteryPercent
  );
  return allowed && otaOfferConsume(offer, &runtime.offer);
}

}  // namespace

void otaUpdatePoll(OtaOfferState* offer, const OtaUpdateRuntimeInputs& inputs) {
  runtime.inputs = inputs;
  if (!runtime.active) {
    if (offer && offer->pending) arm(offer);
    return;
  }
  if (otaUpdateTerminal(runtime.machine)) {
    if (!runtime.terminalSinceMs) cleanupTerminal();
    if (millis() - runtime.terminalSinceMs > 2500) {
      runtime.active = false;
      otaDisplayMetadataClear(&runtime.display);
    }
    return;
  }

  bool confirm = false;
  bool cancel = runtime.cancelRequested;
  if (runtime.confirmRequested || runtime.automatic) {
    if (consumeOfferAfterAuthorization(runtime.automatic)) {
      runtime.offerConsumed = true;
      confirm = true;
      setStatus("Preparing update");
    } else {
      cancel = true;
    }
  }
  runtime.confirmRequested = false;
  runtime.cancelRequested = false;

  OtaOfferState* activeOffer = coordinatedOffer();
  uint32_t now = millis();
  OtaUpdateInputs pure = {};
  pure.nowMs = now;
  pure.offerPending = activeOffer && activeOffer->pending;
  pure.bleConnected = inputs.bleConnected;
  pure.wifiProvisioned = inputs.wifiProvisioned;
  pure.wifiOnline = inputs.wifiOnline;
  pure.trustedTime = inputs.trustedTime;
  pure.offerFresh = activeOffer && otaOfferWindowActive(*activeOffer, now) &&
    otaOfferDeadlineActive(now, activeOffer->offerDeadlineMs);
  pure.externalPower = inputs.externalPower;
  pure.batteryKnown = inputs.batteryKnown;
  pure.batteryPercent = inputs.batteryPercent;
  pure.prompt = inputs.prompt;
  pure.transfer = inputs.transfer;
  pure.provisioning = inputs.provisioning;
  pure.passkey = inputs.passkey;
  pure.functional = inputs.functional;
  otaUpdateStep(&runtime.machine, pure, confirm, cancel);
  if (otaUpdateTerminal(runtime.machine)) cleanupTerminal();
}

void otaUpdateConfirm() {
  if (runtime.active && runtime.machine.phase == OTA_PHASE_CONFIRM)
    runtime.confirmRequested = true;
}

bool otaUpdateCancel() {
  if (!otaUpdateCancellationAllowed(runtime.active, runtime.machine) ||
      runtime.cancelRequested)
    return false;
  runtime.cancelRequested = true;
  return true;
}

bool otaUpdateActive() { return runtime.active; }

bool otaUpdateTransferStarted() {
  return runtime.active && runtime.machine.authenticated &&
    runtime.machine.phase >= OTA_PHASE_OPEN_FIRMWARE &&
    runtime.machine.phase < OTA_PHASE_RESTARTING;
}

OtaUpdateView otaUpdateView() {
  OtaUpdateView view = {};
  view.visible = runtime.active;
  view.authenticated = runtime.machine.authenticated;
  view.bootCommitted = runtime.machine.bootCommitted;
  view.terminal = otaUpdateTerminal(runtime.machine);
  view.error = runtime.machine.phase == OTA_PHASE_ERROR;
  view.phase = runtime.machine.phase;
  view.failure = runtime.machine.failure;
  view.cancellable = otaUpdateCancellationAllowed(runtime.active, runtime.machine);
  view.automatic = runtime.automatic;
  view.sizeBytes = runtime.display.visible
    ? runtime.display.sizeBytes : runtime.machine.imageSize;
  view.percent = otaUpdateOverallProgress(
    runtime.machine.phase,
    runtime.machine.receivedBytes,
    runtime.machine.readbackBytes,
    runtime.machine.imageSize
  );
  if (runtime.display.visible) {
    strncpy(view.version, runtime.display.version, sizeof(view.version) - 1);
    view.version[sizeof(view.version) - 1] = 0;
  }
  strncpy(view.status, runtime.status, sizeof(view.status) - 1);
  view.status[sizeof(view.status) - 1] = 0;
  return view;
}
