# Nexus account UI integration

The GUI3 account dropdown and Settings > Nexus page use the Nexus backend from
[PR #3930](https://github.com/lemonade-sdk/lemonade/pull/3930) at
`478e6ccf7a942c552c6dfce58fad389f38cd4325`. The backend includes the HTTP server
refactor from [PR #3771](https://github.com/lemonade-sdk/lemonade/pull/3771), which
the current Nexus listener implementation requires.

The interface keeps the vendored client SDK, server routes, native passkey
commands, packaging support and API client. `NexusAccount.tsx` replaces the
standalone `NexusPanel` and `NexusShortcut` with a compact account dropdown and
Basic/Advanced page. The old `devices-and-mesh` route resolves to `nexus`.
The standalone header inference selector is not mounted by this account UI;
the inference relay and its client helpers remain available.

## Account boundary

Nexus's upstream client treats a **Cluster as an account**, with device-key
memberships authenticated by a passkey. The reference is
[`login_view.dart`](https://github.com/lemonade-sdk/lemonade-nexus/blob/1c5d901b8c996020b9f9555d1484c29804408c00/apps/LemonadeNexusClient/lib/src/views/login_view.dart).
This GUI3 flow follows that model: create an account, join an account by
invitation, or return to this device's existing membership. It does not introduce
an email/password service or a separate database of human users.

`NexusAccountProvider` starts signed out. `NexusSignInDialog` inspects the local
controller without enabling or unlocking it. Submitting the form explicitly
registers a FIDO2 credential when needed, verifies the passkey, creates or joins
the network, and reads the local-only `GET account` summary. The account ID,
membership identity, current device and admin role must be confirmed by the
server before the header or page becomes signed in. An already unlocked mesh
does not sign the client in automatically. The creator's admin badge is based
on returned permissions, not on the selected form action.

`features/nexus/accountSession.ts` owns this sequence. It pins requests to the
controller and API key chosen at the start; a connection change or unexpected
account ID fails verification. A five-second membership refresh updates device
presence and capabilities. Lost access clears the UI session. Signing out clears
only this client's session and visible avatar, preserving the account image, shared server's network
and other clients. No unlock or sign-in request runs on application startup.

The account name is a client-local label chosen by the user, separate from the
device name. `lemonade_nexus_account_labels` retains only the verified account ID
and label, indexed by controller source. It never retains roles, authenticated
state, invitation tokens, PINs or private keys. A saved label pre-fills the next
sign-in; it does not bypass verification.

Profile images belong to the verified account group. Account devices keep the
image bytes in a local SDK cache, scoped to the device identity and account.
Nexus stores and replicates only a signed image fingerprint and logical revision.
The SDK reads that reference after explicit sign-in and downloads matching bytes
from an authorized account device through the encrypted mesh. Downloaded copies
can serve other devices. Sign-out clears the visible image while the device's
existing mesh session and cache remain available. **Remove image** publishes a
signed removal that other devices observe during refresh.

PNG, JPEG and WebP inputs up to 5 MB are center-cropped to a 128 by 128 PNG.
The saved data URL is limited to 128 KiB. External image URLs are rejected.
Images never enter `.env`, local account labels or Lemonade settings. The
local-only bridge signs the account ID, device public key, SHA-256 fingerprint
and revision with the existing SDK identity. It does not export the key or send
image bytes to Nexus servers. Local cached files use hashed account directories
under the device's Nexus data directory.

Changing an image waits for confirmation of its reference. A failed save keeps
the visible image. A lost response retains the staged local copy because the
reference may already be live. Fetches verify a one-use challenge signature,
current account Read/connection rights, image bounds and the exact fingerprint.
No server session token or API key is sent to the serving peer. If no device
with the current image is reachable, the UI shows initials and guidance to
connect a device with a copy. This is distinct from removal; account access and
image editing remain available. Cached copies remain usable when peers go offline.

The companion Nexus reference API and replication patch is prepared against
`lemonade-nexus` commit `e2c619fa8e8052eb5a5669173b5e9876e44e99fd`. All serving
mesh servers and account devices need the update. The vendored SDK records its
local extensions in `UPSTREAM.json`; its pin is unchanged. Older meshes still
support sign-in and show an image update notice. Transient metadata failures
retain the visible image without ending the account session. The server
accepts references only; the earlier unpublished image-storage format is rejected.

References are readable account metadata, separate from client-encrypted chats
and sealed group keys. Nexus checks current account membership and Read/Write
on publication and replication. It retains signed removal revisions. Devices
recheck current access before serving cached bytes. Revoking access cannot
erase copies already downloaded by a previously authorized device. If all
device copies are lost, an authorized writer must select the image again.

This bridge uses the one persistent device identity already owned by `lemond`.
It does not implement the upstream Flutter client's multi-keyring account
switching. A device already linked to an account must use Sign in; creating a
replacement identity is outside this flow. User administration and account/device
settings remain outside this UI. The provider's typed `connection` and `service`
props preserve extension and isolated-test boundaries. Server permissions, not
the displayed role, authorize every operation.

## Basic and Advanced

Basic is the default. The client stores the selected mode in
`lemonade_nexus_view`, without changing server configuration. Basic shows compact
account devices after sign-in. Advanced exposes the server's recursive tree with
a selected-node details panel; the hierarchy component also supports compact
group browsing with breadcrumbs. Groups can contain other groups or device
nodes; a group is not constrained to represent a user. Device
nodes are leaves. The UI has no fixed organizational depth; server ancestry
validation rejects cycles and paths beyond 128 nodes.

Both modes share a 1120px maximum width and reserve the same heading space so
the mode selector stays in place. Nexus uses the shared Settings selection
colors in light and dark themes.

`NexusHierarchy.tsx` loads branches on demand. Arrow keys, Home, End, Enter and
Space navigate the tree. Search covers only discovered branches and says so in
the interface. Counts describe direct children, and nested nodes without known
presence are shown with unknown status rather than marked offline. Invalid tree
responses produce a recoverable error before they reach the view.

Advanced provides group creation, renaming, and group permissions for discovered
device identities. Controls follow server-provided capabilities. The server
independently validates ancestry, passkey-unlocked state, group rights, principal
membership, self-protection and group-owner protection before forwarding a
change to the SDK's existing private API. Assignment editing is withheld when
the full assignment list is not visible. Membership and permission requests are
still subject to federation authorization.

There is no automatic replay of a failed POST: a lost response may follow a
successful write. Group creation is therefore not duplicated automatically after
a transport failure. The UI asks the user to refresh before retrying an ambiguous
save. Failed requests retain the draft and do not add optimistic tree entries.

The server bridge adds these routes beneath every Nexus prefix listed in
`nexus-integration.md`:

| Method and suffix | Operation |
| --- | --- |
| `GET account` | Verified current membership and bounded avatar state; local-only |
| `POST account/avatar` | Replace/remove an image using `{account_id, member_id, device_node_id, revision, image}`; local-only |
| `GET tree` | Current group and its direct children |
| `GET tree/{id}/children` | A descendant group and its direct children |
| `POST tree/{id}/groups` | Create a child group using `{name}` |
| `POST tree/{id}/rename` | Rename a group using `{name}` |
| `POST tree/{id}/permissions` | Set a device assignment using `{member_node_id, admin, permissions}` |

Names must contain non-whitespace text and fit within 128 UTF-8 bytes. Tree
responses expose display fields, capabilities and visible assignments; wrapped
private key material is omitted. Public loopback requests redirect to the
management listener. Remote requests require the existing API-key configuration;
mesh ingress refuses all tree routes. No group deletion or reparenting operation
is introduced by this UI.

Personal account verification requires the desktop app on the server computer.
Windows uses the [system WebAuthn dialog](https://learn.microsoft.com/en-us/windows/win32/api/webauthn/), including Windows Hello and other
passkey options available on that Windows installation. Linux and macOS use
USB FIDO2 security keys. User presence and user verification remain required;
Windows Hello verification stays in the Windows dialog. The passkey dialog is hosted
by the account provider, so signing in from the header works outside the Nexus
page. Browser and remote clients show desktop guidance instead of offering a
verification path they cannot complete. Advanced management of an already
unlocked server still follows the existing listener authorization rules.

Users with verified invitation permission can open **Connect another device**,
name the invited device and copy a single-use code or full token. Invitations
request a ten-minute lifetime. Closing the dialog does not revoke an invitation;
it remains usable until consumption or server expiry.

## Validation

Run `npm run test:nexus-client` from `src/app` to exercise local controller
discovery and restart, API-key isolation, remote management requests, relay
credentials, failed-switch rollback and session-only peer credentials. This test
is also discovered by `npm run test:runtime`, which the app CI workflow runs.
`tests/nexus-account.runtime.cjs` covers create/join/return sequencing, verified
roles, controller changes, cancellation, invitation validation and label-only
persistence using a mock transport. `tests/nexus-tree.runtime.cjs` covers tree
normalization and malformed-response rejection.
`tests/nexus-account.spec.ts` contains browser account and recursive-group
scenarios with mocked passkeys and API responses.

`NexusManagerTest` is registered with the CMake `cpp-ci` test label. It exercises
listener separation, relay authorization, route prefixes, locked state and
restart persistence. See [Nexus integration](nexus-integration.md) for SDK build
requirements and the hardware-backed pairing checks.

Local mock tests and browser fixtures do not establish live account creation,
hardware verification or federation pairing. A packaged native-app end-to-end
test requires the rebuilt server, Windows Hello or a supported FIDO2 security key and an
explicitly configured test network.
