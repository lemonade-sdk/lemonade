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
// The first kernel check that fails for the gfx1151 KFD node whose
// `properties` file reads as given, or nullopt when both pass.
std::optional<utils::SetupFailure> check_kfd_node(const std::string& properties);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<HalogenServer>(); }
}  // namespace halogen

}  // namespace backends
}  // namespace lemon
