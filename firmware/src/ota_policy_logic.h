#pragma once

struct OtaOfferPolicy {
  bool wake;
  bool automatic;
  bool requiresConfirmation;
};

struct OtaPollStartPlan {
  bool arm;
  bool continueSamePoll;
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

inline OtaPollStartPlan otaPollStartPlan(
  bool active,
  bool pending,
  bool signedAuthorized,
  bool automaticEnabled
) {
  if (active) return {false, true};
  OtaOfferPolicy policy = otaOfferPolicy(
    pending, signedAuthorized, automaticEnabled
  );
  // Ask and legacy flows deliberately keep the shared offer live until A.
  // Direct must continue now so no lifecycle poll can revoke the verified
  // offer between policy selection and its move into private runtime state.
  return {pending, pending && policy.automatic};
}
