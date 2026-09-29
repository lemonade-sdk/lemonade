# Lemonade Nexus client SDK

This directory contains the client SDK and its required userspace transport.
It has no nested Git repository, Nexus server, or sidecar executable.

- `include/LemonadeNexusSDK/`: public C++ and C interfaces, plus transport headers.
- `src/`: SDK implementations; `src/Transport/` contains the shared dataplane.
- `crates/`: boringtun and virtual-netstack FFI libraries, each with a Cargo lockfile.
- `cmake/`: SDK dependency and Rust build rules.
- `lemonade.cmake`: integration with Lemonade's build and installers.
- `UPSTREAM.json`: source revision and license provenance.

The SDK is built as a shared library with a C interface. This keeps its OpenSSL
HTTP implementation separate from Lemonade's HTTP implementation. Lemonade
loads this library directly; it does not launch a separate network process.

Build independently with `cmake -S src/LemonadeNexusSDK -B build/nexus-sdk`, then
`cmake --build build/nexus-sdk --target lemonade_nexus_sdk`.
OpenSSL, libsodium, JSON and spdlog packages are used where available; pinned
source dependencies are the fallback. Rust and Cargo 1.85 or newer are required. Set
`NEXUS_CARGO_OFFLINE=ON` when building with a populated distro Cargo registry.
Override dependency sources through the usual `FETCHCONTENT_SOURCE_DIR_*`
variables for builds without network access.

Local SDK extensions expose dataplane activity and close an individual egress
listener when a group member leaves. The transport has an end-to-end test for
selective listener teardown.
