# Nexus integration

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
device without leaving Chat. Health and models reload from the selected server.
Failed switches restore the previous connection. Enter the peer's API key when
prompted, or in Devices & mesh. Peer keys stay in client memory; they are never
sent to the local controller. Temporary mesh forwarding ports are not saved as
connection settings.

Native passkeys use FIDO2 USB security keys with required user presence and user
verification. PIN prompts and cancellation run inside the desktop app on Linux,
Windows and macOS. Linux requires libudev and working hidraw permissions.
Windows Hello and macOS Touch ID are not native backends in this integration.
A matching-origin webview can also use WebAuthn.

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

Identity, account metadata and enabled preference live in `nexus/` under the
server configuration directory. Unix permissions restrict the directory to its
owner. Restart leaves an enabled device locked. A long supervisor pause locks
the mesh. Transient failures reconnect with bounded backoff after a successful
join, without replaying consumed invite tokens.

Virtual TCP port `13305` exposes Lemonade's actual HTTP port. Selected peers get
an SDK egress listener on `127.0.0.1`. Member removal and address changes close
that listener and existing forwarded connections. Remote HTTP requests retain
normal API-key authentication.

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
| POST | `members/{node_id}/role` | `{"role": "admin" \| "member"}` |
| POST | `members/{node_id}/permissions` | `{"permissions": [...]}` on the group node |
| GET, POST | `devices/{node_id}/access` | Device access list; POST `{"member_node_id", "see", "connect"}` |
| POST | `devices/{node_id}/remove` | Removal; needs `delete_node` on the device |
| POST | `egress` | Loopback endpoint for a selected group member |
| GET | `events` | SSE mesh/membership/lock events; `after` cursor, `once=1` snapshot |

Member and access routes send one change at a time to the federation's
`POST /api/tree/node/assignments/{node_id}` through `ln_add_group_member`, which
replaces the member's entry on that node. `lemond` rejects a change to its own
role or group entry, an `admin` name in the permission list, and unknown
permission names before it calls the federation. It maps federation refusals to
403 (not an admin, own admin entry, device outside the group), 400 (unknown
permission) and 404 (missing node).

The public listener returns status directly and redirects other Nexus routes
with HTTP 307 to a separate loopback management listener. The desktop discovers
this listener through `status` and sends the local API key directly. It is
never exposed through the mesh: userspace ingress itself arrives on loopback
and cannot safely be treated as a desktop control request. The listener rejects
nonlocal clients and untrusted browser origins and enforces `LEMONADE_API_KEY`.
Browser views show device information without management controls. Live events
have a five-second polling fallback.

## Building and verification

`LEMONADE_WITH_NEXUS=ON` is the default. `OFF` builds without SDK dependencies and
reports Nexus unavailable. The SDK uses C++20 separately from Lemonade's C++17
core, CMake 3.25.1+, and Rust/Cargo 1.85+. Linux can use system OpenSSL,
libsodium, JSON, spdlog and cpp-httplib 0.18.3+; pinned sources are the fallback.
Windows and macOS bundle SDK dependencies. Linux desktop builds additionally
need `libudev-dev`.

Offline distro builds must supply locked Rust crates through a local Cargo
registry, then pass `-DNEXUS_CARGO_OFFLINE=ON` in `LEMONADE_NEXUS_CMAKE_ARGS`.
Dependency sources can be provided with `FETCHCONTENT_SOURCE_DIR_*`. These
settings do not require a sibling Nexus checkout.

Checks cover SDK compilation on macOS and Ubuntu 24.04 amd64, userspace TCP forwarding and teardown, server
routing/lock/persistence/control isolation, member and access permission
mapping, desktop Rust compilation and tests, and browser management, access
control and mesh inference switching. Real pairing requires a
deployed federation with matching RP configuration, security keys and two
machines. Windows installers and Debian packages require target-platform
validation.
