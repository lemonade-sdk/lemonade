#pragma once

#include <string>
#include <vector>

namespace lemon::utils {

// Shared Origin allow-list for both HTTP (CORS + pre-dispatch) and WebSocket
// upgrade handlers. The WebSocket path previously checked Origin-host against
// the Host header, which isn't DNS-rebinding-safe: an attacker-controlled
// hostname resolving to loopback/LAN sends a matching Host header.
bool is_origin_allowed(const std::string& origin,
                       const std::vector<std::string>& allowed_origins);

} // namespace lemon::utils
