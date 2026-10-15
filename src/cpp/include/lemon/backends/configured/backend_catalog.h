#pragma once

#include "lemon/backends/backend_option_contract.h"
#include "lemon/backends/configured/backend_definition_store.h"
#include "lemon/backends/configured/configured_backend_entry.h"

namespace lemon::backends {

class BackendCatalog {
public:
    explicit BackendCatalog(const std::vector<DefinitionSource>& sources);
    const std::vector<BackendRegistration>& registrations() const { return registrations_; }
    std::shared_ptr<const BackendCatalogData> data() const { return data_; }
    json export_schema(const std::string& recipe = "") const;
    json validate(const std::string& bytes) const;
private:
    std::shared_ptr<BackendCatalogData> data_;
    std::vector<std::shared_ptr<const ConfiguredBackendEntry>> entries_;
    std::vector<BackendRegistration> registrations_;
    std::vector<DefinitionSource> sources_;
    json schema_;
};

std::vector<DefinitionSource> discover_backend_definitions(const std::string& config_dir);
void bootstrap_backend_catalog(const std::string& config_dir, bool builtin_only = false);
const BackendCatalog* backend_catalog();
const BackendCatalog& frozen_backend_catalog();
std::string external_default_version(const std::string& pin_recipe, const std::string& backend);

}
