#pragma once

#include "lemon/backends/backend_effects.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

#include <cstdint>

namespace lemon {
namespace backends {

// Wraps antirez's ds4-server (DwarfStar) as a lemonade backend. ds4-server is
// a self-contained OpenAI-compatible HTTP server for the DeepSeek V4 family;
// lemonade spawns it as a subprocess and proxies requests, exactly like the
// other LLM backends.
class Ds4Server : public WrappedServer {
public:
    struct InstallPlatformSources {
        std::function<std::string()> os;
        std::function<std::string()> rocm_arch;
    };
    static InstallParams get_install_params(const std::string& backend, const std::string& version,
                                            const InstallPlatformSources& sources);
    static InstallParams get_install_params(const std::string& backend, const std::string& version);
    Ds4Server(const std::string& log_level, ModelManager* model_manager,
              BackendManager* backend_manager);
    Ds4Server(const std::string& log_level, ModelManager* model_manager,
              BackendManager* backend_manager, BackendEffects effects);

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
private:
    BackendEffects effects_;
};

namespace ds4 {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* legacy_spec();
const BackendOps* legacy_ops();
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<Ds4Server>(); }
}  // namespace ds4

}  // namespace backends
}  // namespace lemon
