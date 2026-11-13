#pragma once

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <LemonadeNexusSDK/lemonade_nexus.h>

// Decisions shared by lemond and the tray's device-key carrier. Header-only: the tray
// does not link lemonade-server-core.
namespace lemon {
namespace nexus_user_key {

inline bool wants_device_key(const nlohmann::json& status) {
    return status.is_object() && status.value("available", false) && status.value("enabled", false) &&
        status.value("needs_user_key", false);
}

/// True when lemond's `nexus/status` names `pubkey` as the key the network revoked
/// (`revoked_public_key`): its account was deleted, or the device was removed. The key
/// never authenticates again, so the holder removes it from the store and makes a new one.
inline bool key_revoked(const nlohmann::json& status, const std::string& pubkey) {
    return !pubkey.empty() && status.is_object() && status.value("revoked_public_key", std::string()) == pubkey;
}

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

inline std::string store_failure_text(int code) {
    switch (code) {
    case LN_ERR_NOT_FOUND: return "no device key is stored for this user";
    case LN_ERR_AUTH: return "the OS keystore is locked or refused access";
    case LN_ERR_UNSUPPORTED: return "no usable OS keystore in this session";
    case LN_ERR_REJECTED: return "the stored device key failed verification; remove it and create it again";
    default: return "the device key store failed (SDK error " + std::to_string(code) + ")";
    }
}

}
}
