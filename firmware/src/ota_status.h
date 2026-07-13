#pragma once

#include <stdint.h>

#include "ota_manifest_logic.h"
#include "ota_update.h"

void otaStatusBindOffer(const OtaOfferState& offer);
void otaStatusAwaitConfirmation();
void otaStatusReject(const char* nonce, uint32_t generation, const char* error);
bool otaStatusReplyRunning(const char* nonce, uint32_t generation);
bool otaStatusCancel(const char* nonce, uint32_t generation);
void otaStatusPoll(const OtaUpdateView& view);
