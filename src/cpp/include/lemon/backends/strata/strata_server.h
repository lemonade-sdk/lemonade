#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

class StrataServer : public WrappedServer, public ISlotsServer {
public:
    StrataServer(const std::string& log_level, ModelManager* model_manager,
                 BackendManager* backend_manager);
    ~StrataServer() override;
    static InstallParams get_install_params(const std::string& backend, const std::string& version);
    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;
    json chat_completion(const json& request) override;
    json responses(const json& request) override;
    json get_slots() override;
    json slots_action(int slot_id, const std::string& action, const json& request_body) override;

private:
    std::string runtime_dir_;
};

namespace strata {
std::string engine_asset(const std::string& platform, const std::string& backend,
                         const std::string& arch);
std::vector<BackendAsset> additional_assets(const std::string& backend);
bool supported_checkpoint(const std::string& checkpoint);
json checkpoint_selection(const std::string& checkpoint);
void validate_options(int ctx_size, const std::string& custom_args, int parallel, bool embedded_mtp);
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<StrataServer>(); }
}  // namespace strata
}  // namespace backends
}  // namespace lemon
