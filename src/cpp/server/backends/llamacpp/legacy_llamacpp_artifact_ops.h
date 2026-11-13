#pragma once

#include "lemon/backends/backend_ops.h"

namespace lemon::backends::llamacpp {

class LegacyLlamaCppArtifactOps : public BackendOps {
public:
    void populate_metadata(ModelInfo& info, const BackendOpsContext& context) const override;
    std::string resolve_checkpoint_path(const ModelInfo& info, const CheckpointResolveContext& context) const override;
    std::string find_imported_checkpoint(const std::string& import_dir) const override;
    std::string validate_registration_checkpoint(const std::string& checkpoint) const override;
    std::string validate_checkpoint_file(const std::string& resolved_path) const override;
};

const BackendOps* legacy_llamacpp_artifact_ops();

}
