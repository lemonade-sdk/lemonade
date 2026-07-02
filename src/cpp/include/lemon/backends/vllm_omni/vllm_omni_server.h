#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <filesystem>
#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

// vLLM-Omni backend. Launches the bundle's `vllm-omni-server` on an omni model
// with a single-GPU deploy config and forwards OpenAI-compatible chat requests.
// Native voice / vision ride through transparently: audio output comes back as
// a second choice (`choices[1].message.audio.data`) in the chat response.
class VLLMOmniServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    VLLMOmniServer(const std::string& log_level,
                   ModelManager* model_manager,
                   BackendManager* backend_manager);

    ~VLLMOmniServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation (audio/vision flow through the chat body).
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

private:
    std::filesystem::path rocm_shim_dir_;
    int64_t max_model_len_ = 0;
};

namespace vllm_omni {
// Factory for the vllm-omni backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace vllm_omni
}  // namespace backends
}  // namespace lemon
