#pragma once

#include "backend_definition_runner_internal.h"

namespace lemon::backends {

class ConfiguredV2BackendOps final : public BackendOps {
public:
    explicit ConfiguredV2BackendOps(std::shared_ptr<const DefinitionRunner> runner) : runner_(std::move(runner)) {}
    void resolve_runtime_options(const ModelInfo& model, RecipeOptions& options) const override;
    std::string resolve_checkpoint_path(const ModelInfo&, const CheckpointResolveContext&) const override;
    bool is_downloaded(const ModelInfo&, const BackendOpsContext&) const override;
    void download_model(const ModelInfo&, bool, DownloadProgressCallback, const BackendOpsContext&) const override;
private:
    const std::shared_ptr<const DefinitionRunner> runner_;
};

}
