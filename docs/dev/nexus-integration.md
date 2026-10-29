# Nexus integration

For this GUI3 checkout, see [Nexus account UI](nexus-account-ui.md) for the
account dropdown, passwordless sign-in, Basic/Advanced views and recursive
group-management routes.
The UI walkthrough below describes the original integration panel.

Lemonade links the client SDK vendored in `src/LemonadeNexusSDK`: the files of
one pinned Nexus commit, in Nexus's own layout, plus a patch series (see
[Updating the SDK](#updating-the-sdk)). Lemonade's build rules for it live in
`cmake/NexusSDK.cmake` and `cmake/nexus/`. There is no Nexus server, submodule,
nested repository, or networking sidecar. The shared C
interface isolates SDK OpenSSL from Lemonade's HTTP implementation.

## GUI3

Open **Settings → Devices & mesh**, or **Nexus** in the header. The panel shows
your network, device name, mesh address, members, online state, tier and
latency where available. Each group member is one device. Create or join a
group, invite devices, remove devices, and lock or disable network access.

### Tiers and roles

`lemond` joins as a Tier 3 inference node. It calls `ln_set_inference_node`
before `ln_join_network`. Other client devices join as Tier 4 clients. The
federation records the tier when it creates the device's endpoint node, so a
device that joined before this change keeps its recorded tier. The panel reads
the tier from each device node's `is_inference` field.

An admin holds `admin` on the group node. The group creator is always an admin;
the federation restores that grant on the creator's next join. Admins see an
ADMIN badge and a **Manage access** action on each device:

- **Role** promotes a member to admin or demotes an admin to member. An admin
  cannot change its own role; the control is disabled with an explanation.
- **Group permissions** sets the member's permissions on the group node. `read`
  is always kept. Admins hold every permission.
- **Who can use this device** sets **See** (`read`) and **Connect**
  (`connect_private`) on the device node for each other member. Connect implies
  See. See only shows the device. To reach a device over the mesh, a member
  needs Connect, `connect_shared` or `admin` on that device: the federation
  lists only those devices as the member's mesh peers, and the federation server
  forwards mesh packets only between such pairs. A device the member cannot
  reach is reported offline, and the inference relay refuses it.

The federation returns a node's full assignment list only to a caller with
`edit_node` on that node. Members without it see only their own entry, and an
admin without `edit_node` on a device sees a notice instead of that device's
access list. Removing a device requires `delete_node` on the device node.

Inference server selection is not exposed in the UI yet. `lemond` provides the
[inference relay](#inference-relay), and `selectNexusInferenceServer(nodeId, key?)`
in `src/app/src/features/nexus/nexus.ts` implements the client side: `'local'`
restores the Local Server connection and drops the peer key; another node ID
connects through `<server>/api/v1/nexus/peers/{node_id}` with that peer's key.
`isNexusInferencePeer` selects the online Tier 3 devices to offer. The client
holds peer keys in memory and sends them in the relay header only with requests
to that relay URL, and `lemond` forwards them to that peer alone. Any change to
the connection settings drops the peer key. Relay URLs are not saved as
connection settings.

Native passkeys require user presence and user verification. Windows uses the
system WebAuthn dialog for Windows Hello and other available passkey options;
verification stays in the Windows dialog and cancellation is forwarded to it.
Linux and macOS use FIDO2 USB security keys with PIN prompts and cancellation
inside the desktop app. Linux requires libudev and working hidraw permissions.
macOS Touch ID is not a native backend in this integration. A matching-origin
webview can also use WebAuthn.

A server without a person at it unlocks with its device key instead. The device
key is the Ed25519 identity in `nexus/identity.json`, which already
authenticates every federation session. `POST key/unlock` on the local
controller authenticates with it, creates the account on first use, and marks
the device as key-unlocked. From then on the device unlocks again by itself
after a restart or a supervisor pause, and the passkey routes answer 409. A
device unlocks with a passkey or with its device key, never both. An explicit
`lock` holds until the next `key/unlock` or restart. Whoever can read
`identity.json` can unlock the device, so key unlock belongs on servers whose
disk access already equals control of lemond.

### Remote management

GUI3 can manage a `lemond` on another machine. Connect to the server's address
with its API key, then open **Devices & mesh**. The panel uses the server's
public listener for status, devices, members, roles, group permissions, device
access, invites, joining and device removal. The server must have
`LEMONADE_API_KEY` set; otherwise it refuses these routes and the panel shows
the reason.

Some actions need the desktop app on the server itself: passkey registration
and unlock, turning the mesh on or off, locking, renaming the device and live
events. Unlock the server locally before you manage it remotely. A remote join
works only when the server is unlocked and not yet in a group.

An unlocked device creates and cancels invites without another passkey check.
The local controller rules or the API key authorize the request. A locked device
refuses invites.

Invites come from the federation. Eight-digit codes work when the deployed
federation supports them; older versions return full `lnk_…` tokens, which the
UI can copy and join with. Closing an invite does not revoke it: it remains
single-use until its server expiry.

## Configuration and lifecycle

Without a configured bootstrap host, `lemond` discovers one: it resolves
`<region>.seip.<domain>`, reads each server's certificate name, and remembers
the first name below the domain in `account.json` (`server`). Later starts use
the remembered name and need no network before the SDK's own verified server
directory takes over. The servers' certificates name their own FQDNs, not the
network apex, so the apex is never a valid bootstrap host.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LEMONADE_NEXUS_DOMAIN` | `lemonade-nexus.io` | Network DNS domain for discovery |
| `LEMONADE_NEXUS_REGION` | `us-west` | SEIP region label for discovery |
| `LEMONADE_NEXUS_SERVER` | discovered | Bootstrap HTTPS host; skips discovery |
| `LEMONADE_NEXUS_PORT` | `9100` | Bootstrap HTTPS port |
| `LEMONADE_NEXUS_RP_ID` | from `/api/health` | WebAuthn RP ID; the network's own value unless overridden |
| `LEMONADE_NEXUS_GENESIS_PUBKEY` | discovered | Federation Genesis public key (base64); overrides first-connect discovery |

A configured bootstrap hostname must match its HTTPS certificate: use an
advertised server FQDN. The RP ID is the network's, independent of the
hostname used to reach a server; `lemond` reads it from `/api/health` for the
passkey challenge when it is not configured.

Identity, account metadata and enabled preference live in `nexus/` under the
server configuration directory. Until the user names the device, `status`
reports the machine's host name as `device_name` (without a `.local` suffix),
and a join uses it. Unix permissions restrict the directory to its
owner. Restart leaves an enabled passkey device locked, and a long supervisor
pause locks the mesh; a key-unlocked device unlocks again by itself in both
cases. Transient failures reconnect with bounded backoff after a successful
join, without replaying consumed invite tokens. A session is bound to the
Tier 1 server that issued it; when that server leaves Tier 1 at an epoch
boundary, the SDK moves the session to a current server by itself with the
device identity and `lemond` logs the move. When the SDK cannot move it, it
reports `needs_rejoin` once it has dropped the session, and `lemond` joins
again through a current server, with no passkey prompt. The refusal texts
alone do not start a join.

Virtual TCP port `13305` forwards to a separate loopback listener that
`NexusManager` starts once; a main-port rebind does not touch it. That listener
gets `lemond`'s request middleware, API routes and web UI, so it serves the
normal API with normal API-key authentication, but it refuses every Nexus route.
The `/internal/*` control routes (shutdown, config, cache cleanup, model sync,
aliases) are not registered on it, so a peer gets 404 whether or not an admin
API key is set.
It does not accept WebSocket upgrades. Relayed peers get an SDK egress listener on
`127.0.0.1`. Member removal and address changes close that listener and
existing forwarded connections. Remote HTTP requests retain normal API-key
authentication.

### Route policy

The Nexus SDK decides which mesh peer may call which `lemond` route family at
the receiving device. `lemond` only describes its routes: it builds a catalog
from the route registry, registers it with the SDK for virtual port `13305`
after every join (`ln_mesh_service_set_catalog`), and logs the refusals the SDK
reports (`ln_set_ingress_denied_callback`). A refused request never reaches
`lemond`; the SDK answers 403 itself. `lemond` makes no authorization decision
from the peer headers.

Every route carries a family, set where the route folder registers it
(`RouteRegistry::add(route, family)`; `RouteSpec::family`). A route without a
valid family fails at startup, and so do two families that claim one method and
URL. `nexus_access::kFamilies` lists the families:

| Family | Routes |
| --- | --- |
| `inference` | chat/completions, completions, responses, embeddings, audio, images, 3d and audio generations, classify, rerank, tokenize, slots (read), the Anthropic messages route, the Ollama chat, generate, embed, embeddings, tags, show, ps and version routes, models list and model detail reads (`models`, `models/{id}`, `models/{id}/files`, `models/{id}/options` GET), health, live, stats |
| `mcp` | `/mcp` RPC and SSE |
| `models_manage` | pull, pull/variants, registry/search, delete, load, unload, models/register, models/check-updates, `models/{id}/options` POST and DELETE, downloads and downloads/control, Ollama pull, delete, create, copy, push and blobs |
| `backends` | install, install/dry-run, uninstall, cloud/auth, `cloud/auth/{provider}` |
| `jobs` | jobs create, list, get, delete, pause, resume, interrupt |
| `internal` | every `/internal/*` route; the mesh ingress listener still does not mount them |
| `system` | docs, `docs/{page}`, system-info, system-stats, metrics |
| `management` | routes with no other family: routing/validate, `slots/{id}` actions |

`nexus_access::service_catalog(registry.list(), LEMON_VERSION_STRING)` builds:

```json
{"version": 202604600300,
 "app": {"name": "lemonade", "version": "2026.46.3"},
 "families": [{"name": "inference", "routes": [
   {"methods": ["POST"], "path": "/api/v1/chat/completions"},
   {"methods": ["GET"], "prefix": "/api/v1/models/"}]}]}
```

A route with a `{parameter}` becomes the prefix before it; other paths are
exact, one entry per URL prefix (`/api/v0/`, `/api/v1/`, `/v0/`, `/v1/`).
The version packs `lemond`'s `YEAR.WEEK.N` version as
`YEAR * 10^6 + WEEK * 10^3 + N`, times 100, plus
`nexus_access::kCatalogRevision`. Bump the revision (below 100) when a route's
family changes within one `lemond` version; a release changes the version by
itself, so the revision may return to 0 then. Two development builds of one
release share a catalog version unless the revision changes.

`GET /api/v1/nexus/status` reports `catalog_version`, `app` and
`route_policy`: `"enforced"` when the SDK accepted the catalog,
`"unavailable"` when it refused the catalog (`catalog_error` names the SDK
error) or `lemond` runs without a catalog. `lemond` registers the catalog with
`ln_mesh_service_set_catalog` on every join and receives refusals through
`ln_set_ingress_denied_callback`.

Each refusal event becomes one log line, `WARNING` for `denied` and `INFO` for
`unregistered` and `unattributed`:

```
Nexus route policy: denied POST /api/v1/chat/completions from <peer_node_id> (family inference, needs svc:inference, catalog 202604600300)
```

The line never carries a body, key or address; control characters are replaced
and long fields are cut. The callback runs on an SDK thread, copies the fields
it logs, releases the event with `ln_free` and makes no other SDK call.

Permissions on the tree gain an opaque `svc:<family>` namespace
(`[a-z0-9][a-z0-9_.-]{0,62}` after `svc:`). A group entry's `svc:` names apply
to every device in the group; `admin` implies all. `members/{id}/permissions`,
`tree/{id}/permissions` and invites accept them next to the tree names.
`members/{id}/role` and an invite's `role` expand a preset
(`nexus_access::role_permissions`):

| Role | Group entry |
| --- | --- |
| `administrator` (formerly `admin`) | `read`, `connect_private`, `admin`, `svc:` for every family |
| `moderator` | `read`, `write`, `add_child`, `connect_private`, `svc:inference`, `svc:mcp`, `svc:models_manage`, `svc:jobs` |
| `user` (formerly `member`) | `read`, `connect_private`, `svc:inference` |

A role replaces the member's group entry. An invite takes `role` or
`permissions`, not both; `administrator` carries `admin`, which an invite
cannot grant, so an invite expands `moderator` or `user`. The federation reduces
invite permissions to what the inviter holds. A federation that does not know
the `svc:` names answers the assignment with `unknown permission`, which
`lemond` maps to 400.

An allowed request arrives at the mesh ingress listener with the headers the
SDK set: `X-Nexus-Peer-Node`, `X-Nexus-Peer-Key`, `X-Nexus-Peer-Permissions`,
`X-Nexus-Family` and `X-Nexus-Catalog-Version`. They are trusted on that
listener only: `RequestMiddleware` records the peer node and family for the
request (`nexus_access::request_peer()`, shown in the access log) and removes
the headers on the public and management listeners before any route or
session-header setting can read them.

### Inference relay

`lemond` relays any request under
`/api/v1/nexus/peers/{node_id}/<path>` (also the other route prefixes) to
`<path>` on that device's virtual port `13305`. GUI3 uses
`<server>/api/v1/nexus/peers/{node_id}` as its base URL, so health, models,
chat completions and other HTTP routes reach the peer unchanged. The relay uses
lemond's backend HTTP client (`utils::HttpClient`) for `GET` and `POST`. A
`POST` whose JSON body sets `"stream": true` is streamed with
`utils::HttpClient::post_stream`, as streamed backend responses are: chunks are
written to the client as they arrive, and a slow client slows the transfer
instead of buffering it. The peer's status is read before the response is
committed. A peer that answers with a non-2xx status before its first body byte
gets that status and body relayed as-is, not a 200 stream. A stream that breaks
after the first byte ends with one SSE event,
`event: error` with `data: {"error": {"message", "type", "status_code": 502}}`,
and no `[DONE]`. Other requests return the peer's status and body. Multipart
uploads are rebuilt for the peer. Other methods and WebSocket routes are not
relayed.

- `Authorization` authenticates the caller to this `lemond` and is never
  forwarded.
- `X-Lemonade-Peer-Authorization: Bearer <peer key>` is sent to the peer as its
  `Authorization`.
- Loopback clients follow the normal API-key rules. Other machines need
  `LEMONADE_API_KEY` set on this server and a matching bearer token, as for
  remote management.
- The mesh ingress listener refuses the relay, so a peer cannot relay through
  this device.
- The target must be another group member that is a Tier 3 inference node,
  online and with a mesh address. Otherwise the relay returns 400 (this
  device), 404 (not in the group) or 409 (offline or not an inference node).
  An unreachable peer returns 502.

### Selection port

The SDK serves the mesh port on `127.0.0.1:13306` (`ln_mesh_port_*`). `lemond`
starts it once the mesh is up, with the mesh ingress listener as this device's
local service, and stops it when the mesh locks or turns off. The mesh ingress
listener is always on loopback and applies `lemond`'s API-key policy, so a
request that the port serves locally passes the same checks as on the main
port. Routing by device name, TLS with the group certificate
`*.<group-label>.lo.<base>` and its renewal, the selection and the auto router
are the SDK's; see the Nexus `Client-SDK.md`. When another program, such as the
Lemonade Nexus desktop app, already owns the port, `lemond` keeps running and
`status` reports the conflict in `mesh_port.error`.

`GET`/`POST /api/v1/nexus/selection` with
`{"target": "local" | "auto" | "<node_id>" | null, "peer_key": "..."}` wraps
`ln_mesh_port_set_selection`. The management-route rules apply, and mesh
peers cannot reach it. A missing device answers 404, a device this device may
not connect to answers 403, and a stopped port or a target that cannot be
selected answers 409. `status` includes the SDK's `mesh_port` report: port,
TLS state, selection and the URL of each reachable device.

### Tier 1 servers and the Genesis key

The SDK sends tree, session and private-API calls only to Tier 1 servers that
it verifies from the network's Genesis public key. On first connect without a
key, `lemond` calls `ln_discover_genesis_key`: the SDK asks at least three
hosts in the network's DNS domain over verified HTTPS and accepts the key only
when they all agree and it matches the reported network id. `lemond` stores the
key in `account.json` and pins it on every later start; discovery never
replaces a pinned key. This is trust on first use: it rests on verified-HTTPS
domain ownership and agreement of several hosts.

`LEMONADE_NEXUS_GENESIS_PUBKEY` overrides discovery for another federation.
`lemond` also stores the verified Tier 1 epoch in `account.json`
(`tier1_epoch`) after a join and whenever a poll sees it advance, and passes
it to the SDK at start, so a restart refuses an older membership list. If
discovery fails, those calls fail and `status` reports
`genesis_key_set: false`.

## Control API

Routes are available under `/api/v0/nexus/`, `/api/v1/nexus/`, `/v0/nexus/`,
`/v1/nexus/`, and `/api/nexus/` compatibility aliases.

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `status`, `join/status` | Network/device state and local controller URL |
| GET | `passkey/challenge` | Federation challenge (`register=1` for enrollment) |
| POST | `passkey/register`, `passkey/unlock` | Register or verify the device's credential |
| POST | `key/unlock` | Unlock with the device key (`{defer_join?}`); local-only, refused once a passkey is registered |
| POST | `device/check` | Advisory check that a device name folds to a free DNS label: `{name, code?}`; uses this account's group when joined, else the invite code's group. Answers `{label, available, conflicts, suggestion}`. The federation still refuses a collision on join or rename |
| POST | `enable`, `disable`, `lock`, `device` | Local state and device name |
| POST | `invites`, `invites/cancel`, `join` | Invite a device into this account's top-level group with `{device_name, permissions? \| role?, ttl_sec?}`, or join as a new device with `{code}` |
| POST | `tree/{id}/invites` | Invite into group `{id}` with `{permissions? \| role?, ttl_sec?, device_name?}`; needs `add_child` or admin above it |
| POST | `invites/accept` | Join another network: accept `{code}` as an account that already joined |
| GET | `memberships` | Groups this identity belongs to (`role` is `home` or `member`) and `group_node_id` |
| POST | `memberships/{id}/leave` | Leave a group entered through an invite |
| POST | `tree/{id}/move` | Move device or group `{id}` under `{parent_id}` |
| POST | `account/delete` | Delete this account permanently with `{account_id, member_id, device_node_id, confirm: "delete"}`; local-only |
| GET | `devices` | Group devices with tier, admin flag and permissions |
| GET | `members` | Devices plus `can_manage` (this device is an admin) |
| GET | `account` | Verified current account ID, membership identity, device ID, role and devices; local-only |
| GET | `tree`, `tree/{id}/children` | Current group or a descendant group and direct children |
| POST | `tree/{id}/groups`, `tree/{id}/rename` | Create a child group or rename a group with `{name}` |
| POST | `tree/{id}/permissions` | Group assignment with `{member_node_id, admin, permissions}` |
| POST | `members/{node_id}/role` | `{"role": "administrator" \| "moderator" \| "user"}`; `admin` and `member` still name the first and last preset (see [Route policy](#route-policy)) |
| POST | `members/{node_id}/permissions` | `{"permissions": [...]}` on the group node, tree names and `svc:` names |
| GET, POST | `devices/{node_id}/access` | Device access list; POST `{"member_node_id", "see", "connect"}` |
| POST | `devices/{node_id}/remove` | Removal; needs `delete_node` on the device |
| POST | `egress` | Loopback endpoint for a selected group member |
| any | `peers/{node_id}/<path>` | Relay to a mesh inference device (see [Inference relay](#inference-relay)) |
| GET, POST | `selection` | Selection port target (see [Selection port](#selection-port)) |
| GET | `events` | SSE mesh/membership/lock events; `after` cursor, `once=1` snapshot |

Invite permissions are optional. Without them the federation grants `read` and
`connect_private`, reduced to what the inviter holds. Otherwise they are a list
from `read`, `write`, `add_child`, `connect_private`, `connect_shared` and the
`svc:<family>` names, or a `role` that expands to such a list.
`lemond` refuses `admin` and other names with 400. The reply carries the
`link_token`, `target_node_id`, `permissions` and `expires_at`. A new device
joins with the token through `join`. An account that already joined accepts it
through `invites/accept` and becomes a member of that group; nothing it owns
moves. Tree routes also accept nodes under a membership group, so the desktop
can show, invite into and move into a network it joined. The federation checks
every permission. A move keeps the node's ID, tunnel address and own
assignments, and grants nothing in the new group.

`account/delete` is irreversible. When the federation answers `deleted`,
`lemond` removes `identity.json`, `account.json`, the account image cache and
the certificate directory, then turns the mesh off. A `pending` answer means
Tier 1 has not committed the deletion yet; the local files stay.

Member and access routes send one change at a time to the federation's
`POST /api/tree/node/assignments/{node_id}` through `ln_add_group_member`, which
replaces the member's entry on that node. `lemond` rejects a change to its own
role or group entry, an `admin` name in the permission list, and unknown
permission names before it calls the federation. It maps federation refusals to
403 (not an admin, own admin entry, device outside the group), 400 (unknown
permission) and 404 (missing node).

Each listener applies its own rules:

| Listener | Client | Nexus routes |
| --- | --- | --- |
| Public | Loopback | `status` and the relay directly; every other route redirects with HTTP 307 to the management listener |
| Public | Other machines | `status`, `join/status`, `devices`, `members`, `members/{id}/role`, `members/{id}/permissions`, `devices/{id}/access`, `devices/{id}/remove`, `invites`, `invites/cancel`, `join`, the tree routes, `invites/accept`, `memberships`, `memberships/{id}/leave` and the relay, only with `LEMONADE_API_KEY` set and `Authorization: Bearer <key>` |
| Management (`127.0.0.1`, ephemeral port) | Local desktop | All routes |
| Mesh ingress (`127.0.0.1`, ephemeral port) | Mesh peers | None (HTTP 403); `/internal/*` is not registered (HTTP 404) |

On the public listener, a remote request gets HTTP 403 when no
`LEMONADE_API_KEY` is set, HTTP 401 for a missing or wrong key, and HTTP 403 for
a local-only route. `LEMONADE_ADMIN_API_KEY` is also accepted once
`LEMONADE_API_KEY` is set. Keys are compared in constant time. The public
listener's origin checks still apply. Remote `status` omits `controller_url`
and reports `remote_management`.

`account` is a local management operation and is refused on remote and mesh
ingress requests. It requires an unlocked, joined session and known current
membership permissions. It contains no private key material or client display
label; those labels belong to the GUI3 client.

A federation that cannot be reached is an outage, not a refusal. When a
connection fails or times out, the federation answers 5xx, or no verified Tier 1
server is reachable, every route answers 503 with `"retryable": true` in the
JSON body. The device stays unlocked and joined, and `status` reports
`"status": "degraded"` with the error text and `"error_retryable": true` until
the next successful refresh. A refused session answers 403; the SDK's C ABI
reports a federation 401 and 403 alike. A client should end its signed-in
session on 401 or 403 only, never on 503.

The desktop discovers the management listener through `status` and sends the
local API key directly. It rediscovers the listener once when a request cannot
reach it, because `lemond` picks a new port each time it starts. The management
listener accepts only loopback clients and gets the same origin, CORS and
API-key handling as the public listener: `RequestMiddleware::install`. Mesh traffic never
reaches the public or management listener: userspace ingress arrives on
loopback and cannot safely be treated as a desktop control request, so it has a
listener of its own. Browser views on this device show device information
without management controls. Live events have a five-second polling fallback.

## Building and verification

`LEMONADE_WITH_NEXUS` defaults to `ON` when Cargo 1.85 or newer is found, and
to `OFF` otherwise. CMake looks in `PATH`, then in `/usr/lib/rust-1.*/bin`,
where Ubuntu's versioned `cargo-1.xx` packages install. Set
`LEMONADE_CARGO_EXECUTABLE` to choose one. An explicit `ON` without that
toolchain fails at configuration. `OFF` builds without SDK dependencies and
reports Nexus unavailable.

Rust 1.85 is a real minimum: the locked `zeroize` 1.9 uses edition 2024, and
Cargo 1.75 cannot read the version 4 lockfiles. Ubuntu 24.04's `cargo` is 1.75;
its `cargo-1.85` package from noble-updates builds both crates. Debian 13 ships
Cargo 1.85 and Ubuntu 26.04 ships 1.93 as `cargo`.

The SDK uses C++20 separately from Lemonade's C++17
core, CMake 3.25.1+, and Rust/Cargo 1.85+. Linux can use system OpenSSL,
libsodium, JSON, spdlog and cpp-httplib 0.18.3+; pinned sources are the fallback.
Windows and macOS bundle SDK dependencies. Linux desktop builds additionally
need `libudev-dev`.

Offline builds set `LEMONADE_NEXUS_CARGO_VENDOR_DIR` to a `cargo vendor`
directory that holds every locked crate; Cargo then runs with `--offline`.
The Debian package does this: `debian/vendor-rust-crates` vendors both crates
into `debian/rust-vendor` after the build dependencies are installed and before
`dpkg-buildpackage`, so the source package carries them for Launchpad. The
directory is about 120 MB (18 MB compressed) after the script drops the Windows
import libraries, and it is never committed. The Debian build uses the system
OpenSSL, libsodium, spdlog, JSON and cpp-httplib packages, so it needs no other
downloads. Other dependency sources can be provided with
`FETCHCONTENT_SOURCE_DIR_*`. These settings do not require a sibling Nexus
checkout.

Checks cover SDK compilation on macOS and Ubuntu 24.04 amd64, userspace TCP forwarding and teardown, server
routing/lock/persistence/control isolation, remote management authorization and
mesh ingress refusal, member and access permission mapping, desktop Rust compilation and tests, and browser management, access
control and mesh inference switching. Real pairing requires a
deployed federation with matching RP configuration, security keys and two
machines. Windows installers and Debian packages require target-platform
validation.

## Updating the SDK

`src/LemonadeNexusSDK/UPSTREAM.json` pins one Nexus commit (full SHA) and lists
the vendored paths. Lemonade-only changes are `git format-patch` files in
`src/LemonadeNexusSDK/patches/`, applied in order. No other change to the
vendored tree is allowed; send fixes to Nexus so the series can shrink.

1. Run `python tools/update_nexus_sdk.py <commit-or-tag>`. Add
   `--source <path>` to use a local Nexus clone. The script fetches the commit,
   copies the profile paths into a clean tree, applies the patches (and stops on
   any conflict), installs the result, rewrites `UPSTREAM.json` and prints a
   summary.
2. If a patch no longer applies, rebase it onto the new commit, regenerate it
   with `git format-patch`, and run the script again. Drop a patch once Nexus
   carries the change.
3. Build `lemond` and `test_nexus_manager` with `LEMONADE_WITH_NEXUS=ON`, and
   check that every `ln_*` symbol `lemond` imports is still exported.

CI runs `python tools/update_nexus_sdk.py --check`. It rebuilds the pinned
commit plus the patches in a temporary directory and fails if the result
differs from the committed tree, so a hand edit cannot slip in.
