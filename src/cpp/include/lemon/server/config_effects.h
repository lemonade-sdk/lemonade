#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "lemon/server/server_context.h"

namespace lemon {

// A config key with a runtime effect gets one branch here, applied both when lemond
// starts and whenever POST /internal/set changes the key.
class ConfigEffects {
public:
    explicit ConfigEffects(ServerContext& ctx) : ctx_(ctx) {}

    // value is what /internal/set changed under key; a nested section carries only its
    // changed sub-keys.
    void apply(const std::string& key, const nlohmann::json& value = nullptr);

private:
    // Hot-swaps a backend binary when its *_bin value changes: unloads the models using
    // it, reinstalls, and reloads them. Errors are logged, never raised, since the config
    // change has already been applied.
    void apply_bin_change(const std::string& section, const std::string& bin_key,
                          const std::string& new_value);

    ServerContext& ctx_;
};

} // namespace lemon
