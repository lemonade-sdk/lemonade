#pragma once

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>
#endif

// Decisions shared by lemond and the tray's device-key carrier. Header-only: the tray
// does not link lemonade-server-core.
namespace lemon {
namespace nexus_user_key {

/// True when lemond's `nexus/status` asks a process in the user's session for the
/// device key: the SDK is available, the mesh is enabled and no identity is active.
/// A disabled mesh, a build without the SDK and a device with an identity never ask.
inline bool wants_device_key(const nlohmann::json& status) {
    return status.is_object() && status.value("available", false) && status.value("enabled", false) &&
        status.value("needs_user_key", false);
}

/// The loopback port of lemond's management listener from `status.controller_url`
/// ("http://127.0.0.1:<port>"), or 0 when it is absent or malformed.
inline int controller_port(const nlohmann::json& status) {
    if (!status.is_object()) return 0;
    const auto url = status.value("controller_url", std::string());
    const std::string prefix = "http://127.0.0.1:";
    if (url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0) return 0;
    const auto digits = url.substr(prefix.size());
    if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) return 0;
    const long port = std::strtol(digits.c_str(), nullptr, 10);
    return port >= 1 && port <= 65535 ? static_cast<int>(port) : 0;
}

#ifdef LEMONADE_WITH_NEXUS
/// Why ln_identity_user_load or ln_identity_user_create returned no identity, as one
/// sentence for status text and logs. It names the condition, never the store contents.
inline std::string store_failure_text(int code) {
    switch (code) {
    case LN_ERR_NOT_FOUND: return "no device key is stored for this user";
    case LN_ERR_AUTH: return "the OS keystore is locked or refused access";
    case LN_ERR_UNSUPPORTED: return "no usable OS keystore in this session";
    case LN_ERR_REJECTED: return "the stored device key failed verification; remove it and create it again";
    default: return "the device key store failed (SDK error " + std::to_string(code) + ")";
    }
}
#endif

}
}
