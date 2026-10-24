# Nexus integration

For this GUI3 checkout, see [Nexus account UI](nexus-account-ui.md) for the
account dropdown, passwordless sign-in, Basic/Advanced views and recursive
group-management routes.
The UI walkthrough below describes the original integration panel.

Lemonade links the client SDK in `src/LemonadeNexusSDK`. SDK headers, sources,
Rust transport crates and build rules stay in that directory. There is no Nexus
server, submodule, nested repository, or networking sidecar. The shared C
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
  See.

The federation returns a node's full assignment list only to a caller with
`edit_node` on that node. Members without it see only their own entry, and an
admin without `edit_node` on a device sees a notice instead of that device's
access list. Removing a device requires `delete_node` on the device node.

The header's **Inference server** selector chooses this device or a linked mesh
device without leaving Chat. It works in the desktop app and in a browser, on
this device or on another machine. It always offers **Local Server**, also
without Nexus support, with the mesh off or without inference devices, and adds
the group's Tier 3 devices that are online. On screens 768 px wide or less it is
hidden like the Nexus shortcut unless a mesh device is selected; the connection
settings still reach the local server. Health and models reload from the
selected server. Failed switches restore the previous connection, and choosing
Local Server restores the normal connection and drops the peer key.

The client keeps talking to its own `lemond`, which relays the requests to the
selected device over the mesh (see [Inference relay](#inference-relay)). The
selection lives in that client only; other clients of the same `lemond` keep
their own server. Enter the peer's API key when prompted, or in Devices & mesh.
The client holds peer keys in memory and sends them in the relay header only with
requests to that relay URL, and `lemond` forwards them to that peer alone. Any
change to the connection settings drops the peer key. Relay URLs are not saved as
connection settings.

Native passkeys require user presence and user verification. Windows uses the
system WebAuthn dialog for Windows Hello and other available passkey options;
verification stays in the Windows dialog and cancellation is forwarded to it.
Linux and macOS use FIDO2 USB security keys with PIN prompts and cancellation
inside the desktop app. Linux requires libudev and working hidraw permissions.
macOS Touch ID is not a native backend in this integration. A matching-origin
webview can also use WebAuthn.

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

The bootstrap hostname found in Nexus configuration is `lemonade-nexus.io`.

| Variable | Default | Purpose |
| --- | --- | --- |
| `LEMONADE_NEXUS_SERVER` | `lemonade-nexus.io` | Bootstrap HTTPS host |
| `LEMONADE_NEXUS_PORT` | `9100` | Bootstrap HTTPS port |
| `LEMONADE_NEXUS_RP_ID` | `lemonade-nexus.io` | RP ID; must match the federation |

The bootstrap hostname must match its HTTPS certificate. Use an advertised
server FQDN when the bootstrap alias is not covered by that certificate. The
RP ID must match the federation's `/api/health` response; it is independent of
the hostname used to reach that server.

Identity, account metadata and enabled preference live in `nexus/` under the
server configuration directory. Unix permissions restrict the directory to its
owner. Restart leaves an enabled device locked. A long supervisor pause locks
the mesh. Transient failures reconnect with bounded backoff after a successful
join, without replaying consumed invite tokens.

Virtual TCP port `13305` forwards to a separate loopback listener that
`NexusManager` starts once; a main-port rebind does not touch it. That listener
gets `lemond`'s request middleware, API routes and web UI, so it serves the
normal API with normal API-key authentication, but it refuses every Nexus route.
It does not accept WebSocket upgrades. Relayed peers get an SDK egress listener on
`127.0.0.1`. Member removal and address changes close that listener and
existing forwarded connections. Remote HTTP requests retain normal API-key
authentication.

### Inference relay

`lemond` relays any request under
`/api/v1/nexus/peers/{node_id}/<path>` (also the other route prefixes) to
`<path>` on that device's virtual port `13305`. GUI3 uses
`<server>/api/v1/nexus/peers/{node_id}` as its base URL, so health, models,
chat completions and other HTTP routes reach the peer unchanged. The relay uses
lemond's backend HTTP client (`utils::HttpClient`) for `GET` and `POST`. A
`POST` whose JSON body sets `"stream": true` goes through
`StreamingProxy::forward_byte_stream`, the path streamed backend responses use:
chunks are written to the client as they arrive, a slow client slows the
transfer instead of buffering it, and a peer error becomes a JSON error in the
stream. Other requests return the peer's status and body. Multipart uploads are
rebuilt for the peer. Other methods and WebSocket routes are not relayed.

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
  An unreachable peer returns 502, or a JSON error inside a streamed response.

## Control API

Routes are available under `/api/v0/nexus/`, `/api/v1/nexus/`, `/v0/nexus/`,
`/v1/nexus/`, and `/api/nexus/` compatibility aliases.

| Method | Route | Purpose |
| --- | --- | --- |
| GET | `status`, `join/status` | Network/device state and local controller URL |
| GET | `passkey/challenge` | Federation challenge (`register=1` for enrollment) |
| POST | `passkey/register`, `passkey/unlock` | Register or verify the device's credential |
| POST | `enable`, `disable`, `lock`, `device` | Local state and device name |
| POST | `invites`, `invites/cancel`, `join` | Invite or join a group |
| GET | `devices` | Group devices with tier, admin flag and permissions |
| GET | `members` | Devices plus `can_manage` (this device is an admin) |
| GET | `account` | Verified current account ID, membership identity, device ID, role and devices; local-only |
| GET | `tree`, `tree/{id}/children` | Current group or a descendant group and direct children |
| POST | `tree/{id}/groups`, `tree/{id}/rename` | Create a child group or rename a group with `{name}` |
| POST | `tree/{id}/permissions` | Group assignment with `{member_node_id, admin, permissions}` |
| POST | `members/{node_id}/role` | `{"role": "admin" \| "member"}` |
| POST | `members/{node_id}/permissions` | `{"permissions": [...]}` on the group node |
| GET, POST | `devices/{node_id}/access` | Device access list; POST `{"member_node_id", "see", "connect"}` |
| POST | `devices/{node_id}/remove` | Removal; needs `delete_node` on the device |
| POST | `egress` | Loopback endpoint for a selected group member |
| any | `peers/{node_id}/<path>` | Relay to a mesh inference device (see [Inference relay](#inference-relay)) |
| GET | `events` | SSE mesh/membership/lock events; `after` cursor, `once=1` snapshot |

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
| Public | Other machines | `status`, `join/status`, `devices`, `members`, `members/{id}/role`, `members/{id}/permissions`, `devices/{id}/access`, `devices/{id}/remove`, `invites`, `invites/cancel`, `join` and the relay, only with `LEMONADE_API_KEY` set and `Authorization: Bearer <key>` |
| Management (`127.0.0.1`, ephemeral port) | Local desktop | All routes |
| Mesh ingress (`127.0.0.1`, ephemeral port) | Mesh peers | None (HTTP 403) |

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
