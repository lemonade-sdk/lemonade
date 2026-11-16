#pragma once

#include "lemon/model_manager.h"
#include "lemon/models/registry_types.h"

#include <string>

namespace lemon {
namespace test {

// A manager wired the way the server wires one. Presence is asked of the source a
// checkpoint binds to, and a bare "org/repo" checkpoint binds to a host only through the
// default model source, so a manager without that hook answers "not downloaded" for every
// registry model. That is true of the manager and false of a running server, which reads
// the value from config (default: whatever an empty source name means, in registry_types).
inline lemon::ModelManagerConfig server_wired_config(std::string extra_models_dir = {}) {
    lemon::ModelManagerConfig config;
    config.extra_models_dir = std::move(extra_models_dir);
    config.default_model_source = [] {
        return lemon::remote_registry_source_name(lemon::parse_remote_registry_source(""));
    };
    return config;
}

}  // namespace test
}  // namespace lemon
