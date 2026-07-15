#pragma once

#include <string>
#include <vector>

namespace lemon::utils {

// Shared Origin allow-list check used by both the HTTP server (CORS +
// pre-dispatch rejection) and the WebSocket upgrade handler. A single
// implementation keeps the two policies from drifting (see SWSPLAT-24172
// follow-up: the WebSocket path previously had its own, weaker check that
// allowed any Origin whose host matched the request's Host header, which is
// not DNS-rebinding-safe — an attacker-controlled name can resolve to
// loopback/LAN and will send a matching Host header for the same name).
//
// Allowed:
//   - The native desktop-app origins (Tauri custom scheme / WebView2 virtual
//     host).
//   - Loopback origins (http/https) on any port. A remote attacker's page
//     cannot forge these: the browser stamps Origin with its own resolved
//     host, and DNS rebinding still yields the attacker's hostname, not a
//     loopback literal.
//   - Any origin explicitly present in `allowed_origins` (the user-configured
//     allow-list for non-loopback web-app access, e.g. --host 0.0.0.0).
bool is_origin_allowed(const std::string& origin,
                       const std::vector<std::string>& allowed_origins);

} // namespace lemon::utils
