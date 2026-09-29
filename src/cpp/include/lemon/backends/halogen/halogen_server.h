#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/wrapped_server.h"

#include <string>

namespace lemon {
namespace backends {

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

namespace halogen {
// True when the kernel `release` names (uname -r) is Linux 7.0 or newer, or
// cannot be read. Halogen registers its checkpoint with the GPU as a read-only
// file mapping, which needs kernel support that is not backported.
bool kernel_supported(const std::string& release);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<HalogenServer>(); }
}  // namespace halogen

}  // namespace backends
}  // namespace lemon
