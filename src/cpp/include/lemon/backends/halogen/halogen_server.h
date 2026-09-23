#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/container_backend.h"
#include "lemon/wrapped_server.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace backends {

// Runs Peonist's Halogen Flash server image. Unlike the llama.cpp toolboxes,
// the image has its own entrypoint and takes no argv: every setting arrives as
// a HALOGEN_* environment variable.
class HalogenServer : public WrappedServer {
public:
    HalogenServer(const std::string& log_level, ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~HalogenServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

class HalogenOps : public ContainerBackendOps {
public:
    HalogenOps() : ContainerBackendOps("halogen") {}

    InstallCheck check_install(const std::string& backend, bool binary_found) const override;
    void populate_metadata(ModelInfo& info, const BackendOpsContext& ctx) const override;
    std::optional<UnavailableState> classify_unavailable(
        const std::string& backend, const std::string& install_error,
        const std::string& default_install_command) const override;
    std::optional<std::vector<std::string>> select_checkpoint_files(
        const std::string& main_variant,
        const std::vector<std::string>& repo_files) const override;
};

namespace halogen {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<HalogenServer>(); }

// Running kernel major version, or 0 when it cannot be read. Exposed for tests.
int running_kernel_major();
// True when the host kernel is new enough for Halogen's memory path.
bool kernel_supports_halogen();
}  // namespace halogen

}  // namespace backends
}  // namespace lemon
