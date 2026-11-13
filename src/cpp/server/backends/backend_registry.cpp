#include "backend_builtin_seeds_internal.h"
#include "backend_catalog_publication_internal.h"
#include "backend_container_selectors_internal.h"
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
        if (reg.descriptor->recipe == recipe && reg.backend_selector.empty()) {
            return reg.spec;
        }
    }
    return nullptr;
}

const BackendOps* ops_for(const std::string& recipe) {
    for (const auto& reg : all_registrations()) {
        if (reg.descriptor->recipe == recipe && reg.backend_selector.empty()) {
            return reg.ops;
        }
    }
    return default_backend_ops();
}

const BackendRegistration* select_backend_registration(const std::vector<BackendRegistration>& registrations,
    const std::string& recipe, const std::string& backend) {
    const auto id = builtin_container_selector_id(recipe, backend);
    if (!id.empty()) {
        for (const auto& registration : registrations)
            if (registration.descriptor->recipe == recipe && registration.backend_selector == backend)
                return &registration;
        throw std::runtime_error("Builtin backend selector is inactive: " + recipe + ":" + backend);
    }
    for (const auto& registration : registrations)
        if (registration.descriptor->recipe == recipe && registration.backend_selector.empty()) return &registration;
    return nullptr;
}

bool is_inactive_backend_selector(const std::string& recipe, const std::string& backend) {
    if (builtin_container_selector_id(recipe, backend).empty()) return false;
    for (const auto& registration : all_registrations())
        if (registration.descriptor->recipe == recipe && registration.backend_selector == backend) return false;
    return true;
}

void require_active_backend_selector(const std::string& recipe, const std::string& backend) {
    if (is_inactive_backend_selector(recipe, backend))
        throw std::runtime_error("Backend " + recipe + ":" + backend + " is staged and not active in this build");
}

std::unique_ptr<WrappedServer> create_server(const std::string& recipe, const BackendContext& ctx) {
    if (const auto* selected = select_backend_registration(all_registrations(), recipe, ctx.backend_selector)) {
        const auto& reg = *selected;
        std::unique_ptr<WrappedServer> server = reg.create(ctx);
        if (server) {
            if (!server->operation_bindings()) {
                throw std::logic_error("Backend factory returned an instance without operation bindings");
            }
            server->set_descriptor(reg.descriptor);
        }
        return server;
    }
    return nullptr;
}

} // namespace backends
} // namespace lemon
