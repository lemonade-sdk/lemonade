#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon {
namespace backends {

class StrataServer : public WrappedServer {
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

private:
    std::string runtime_dir_;
};

namespace strata {
std::string engine_asset(const std::string& platform, const std::string& backend,
                         const std::string& arch);
std::vector<BackendAsset> additional_assets(const std::string& backend);
bool supported_checkpoint(const std::string& checkpoint);
json build_config(const std::string& model_name, const std::string& gguf,
                  const std::string& executable, const std::string& tokenizer,
                  const std::string& backend, int ctx_size,
                  const std::string& custom_args = "", int parallel = 1);
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<StrataServer>(); }
}  // namespace strata
}  // namespace backends
}  // namespace lemon
