#include "backend_builtin_seeds_internal.h"
#include "backend_catalog_publication_internal.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/wrapped_server.h"

#include <algorithm>

namespace lemon {
namespace backends {

const std::vector<BackendRegistration>& all_registrations() {
    if (const auto* catalog = static_cast<const BackendCatalog*>(catalog_publication().consume_server())) return catalog->registrations();
    return builtin_registrations();
}

const std::vector<BackendRegistration>& builtin_registrations() {
    catalog_publication().server();
    const auto& seeds = builtin_registration_seeds();
    const bool has_v2 = std::any_of(seeds.begin(), seeds.end(), [](const auto& seed) { return !seed.definition_id.empty(); });
    if (!has_v2) {
        static const auto registrations = [&] {
            std::vector<BackendRegistration> rows;
            for (const auto& seed : seeds) rows.push_back(seed.legacy);
            return rows;
        }();
        return registrations;
    }
    static const auto catalog = [] {
        const auto result = build_backend_catalog(freeze_builtin_catalog(), {});
        if (!result.catalog) throw std::logic_error("Builtin catalog finalization failed");
        return result.catalog;
    }();
    return catalog->registrations();
}

const BackendSpec* spec_for(const std::string& recipe) {
    for (const auto& reg : all_registrations()) {
        if (reg.descriptor->recipe == recipe) {
            return reg.spec;
        }
    }
    return nullptr;
}

const BackendOps* ops_for(const std::string& recipe) {
    for (const auto& reg : all_registrations()) {
        if (reg.descriptor->recipe == recipe) {
            return reg.ops;
        }
    }
    return default_backend_ops();
}

std::unique_ptr<WrappedServer> create_server(const std::string& recipe, const BackendContext& ctx) {
    for (const auto& reg : all_registrations()) {
        if (reg.descriptor->recipe == recipe) {
            std::unique_ptr<WrappedServer> server = reg.create(ctx);
            if (server) {
                if (!server->operation_bindings()) {
                    throw std::logic_error("Backend factory returned an instance without operation bindings");
                }
                server->set_descriptor(reg.descriptor);
            }
            return server;
        }
    }
    return nullptr;
}

} // namespace backends
} // namespace lemon
