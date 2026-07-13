# Direct Mac OTA Design

## Decision

Code Buddy will support two device-owned update policies:

- `Ask` (default): a signed Mac update request opens the confirmation screen automatically; A installs and B rejects. The user no longer opens an OTA receive window first.
- `Direct`: after one physical opt-in in Settings, an explicitly requested Mac update starts without A. This is not background update checking.

The first firmware containing this feature still uses the existing manual OTA path. After the user enables `Direct`, a second version update must complete without device navigation or A to prove the workflow.

## Trust Model

BLE ownership and the persisted owner name are not authentication. The USB-bootstrap manifest public key is the update identity.

For firmware versions that support Direct OTA, the Mac signs a canonical authorization envelope containing:

- a domain-separated action and schema;
- the target `Codex-XXXX` device name;
- nonce and generation;
- target version and size;
- manifest and signature URLs;
- issued-at and short expiry timestamps.

The device verifies the envelope with its pinned P-256 key before opening an update UI or making a network request. It rejects bad signatures, wrong devices, stale/future requests, malformed fields, replayed identities, non-newer versions, or unsafe runtime gates. Legacy unsigned offers remain valid only inside the old physical receive window, solely to bootstrap the first compatible release.

The Mac runtime directory is mode `0700`, the agent socket is `0600`, and macOS peer credentials must match the agent user. These controls prevent other local users from invoking the signing service. Same-user Mac compromise remains outside the protection of Direct mode; `Ask` remains available for users who want a physical factor every time.

## Device UX

Settings adds `auto ota` with `off` by default. Changing it is a physical device action and persists in NVS. Factory reset clears it.

The existing `ota update` item remains as a manual/recovery entry point. In normal use:

- `Ask`: the Mac command wakes the update card and shows Install/Cancel.
- `Direct`: the Mac command wakes the update card, labels it as a trusted Mac update, and starts authentication/download automatically.
- B and Mac Ctrl-C cancel only before the boot slot becomes irreversible.
- Progress, target version, size, readback, restart, boot health, and rollback behavior remain visible.

## Data Flow

1. Mac probes the current firmware version over BLE.
2. For compatible firmware, it creates and signs the bounded authorization envelope; older firmware receives the legacy six-field offer.
3. Device validates the signed offer before accepting it.
4. `Ask` waits for A; `Direct` consumes the authorized offer automatically.
5. Device downloads the separately signed manifest and app-only image over one-shot pinned HTTPS.
6. Device writes the inactive slot, reads it back, verifies the hash, commits, reboots, and reports the exact new version with valid boot health.

## Failure Behavior

- Invalid or unauthorized offers are silent and perform no network or flash work.
- Approval, transfer, provisioning, passkey, menu, power, Wi-Fi, or trusted-time conflicts fail closed.
- Before boot commit, failure leaves the current image active.
- After boot commit, failed health validation rolls back automatically.
- Factory reset clears Direct policy, Wi-Fi, BLE bonds, and local device identity state.

## Verification

Tests cover canonical host signing; device signature/device/time/replay checks; default and persisted policy; factory reset behavior; Ask and Direct state transitions; cancellation boundaries; protocol compatibility; local runtime permissions; malformed and unsigned offers; power/conflict gates; HTTPS/hash/readback; build and rollback symbol checks. Final proof requires one manual OTA to the Direct-capable release and one subsequent no-touch Direct OTA.
