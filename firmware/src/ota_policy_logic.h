#pragma once

struct OtaOfferPolicy {
  bool wake;
  bool automatic;
  bool requiresConfirmation;
};

inline constexpr bool otaAutomaticDefault() { return false; }

inline constexpr const char* otaAutomaticNvsKey() { return "s_aota"; }

inline constexpr OtaOfferPolicy otaOfferPolicy(
  bool pending,
  bool signedAuthorized,
  bool automaticEnabled
) {
  // Unsigned offers retain the physical receive-window flow. Only an offer
  // already verified against the pinned device key can wake the formal OTA
  // surface or gain Direct authorization.
  return pending && signedAuthorized
    ? OtaOfferPolicy{true, automaticEnabled, !automaticEnabled}
    : OtaOfferPolicy{false, false, false};
}
