#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

#include <string>

namespace lemon {
namespace backends {

class AmdGpuServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    AmdGpuServer(const std::string& log_level, ModelManager* model_manager,
                 BackendManager* backend_manager);

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

namespace amdgpu {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<AmdGpuServer>(); }
}  // namespace amdgpu

}  // namespace backends
}  // namespace lemon
