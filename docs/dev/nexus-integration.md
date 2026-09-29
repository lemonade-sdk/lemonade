# Nexus integration

Lemonade links the client SDK in `src/LemonadeNexusSDK`. SDK headers, sources,
Rust transport crates and build rules stay in that directory. There is no Nexus
server, submodule, nested repository, or networking sidecar. The shared C
interface isolates SDK OpenSSL from Lemonade's HTTP implementation.

## GUI3

Open **Settings → Devices & mesh**, or **Nexus** in the header. The panel shows
your network, device name, mesh address, members, online state, permissions and
latency where available. Group members represent devices and people. Create or
join a group, invite devices, remove devices as an owner, and lock or disable
network access.

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
| GET | `devices` | Group devices |
| POST | `devices/{node_id}/remove` | Owner-authorized removal |
| POST | `egress` | Loopback endpoint for a selected group member |
| GET | `events` | SSE mesh/membership/lock events; `after` cursor, `once=1` snapshot |

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
routing/lock/persistence/control isolation, desktop Rust compilation and tests,
and browser management and mesh inference switching. Real pairing requires a
deployed federation with matching RP configuration, security keys and two
machines. Windows installers and Debian packages require target-platform
validation.
