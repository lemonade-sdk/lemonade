#pragma once

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_definition.h"

namespace lemon::backends {

class ConfiguredBackendOps final : public BackendOps {
public:
    explicit ConfiguredBackendOps(std::shared_ptr<const BackendDefinition> definition) : definition_(std::move(definition)) {}
    std::string resolve_checkpoint_path(const ModelInfo& info, const CheckpointResolveContext& context) const override;
    std::string find_imported_checkpoint(const std::string& import_directory) const override;
    std::optional<std::vector<std::string>> select_checkpoint_files(const std::string& variant, const std::vector<std::string>& repository_files) const override;
    InstallParams install_params(const InstallContext& context) const;
private:
    const std::shared_ptr<const BackendDefinition> definition_;
};

}
