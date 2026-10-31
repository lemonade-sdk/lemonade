#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/wrapped_server.h"

#include <string>
#include <vector>

namespace lemon {
namespace backends {

class GufoServer : public WrappedServer {
public:
    GufoServer(const std::string& log_level, ModelManager* model_manager,
               BackendManager* backend_manager);
    ~GufoServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

namespace gufo {
// `gufo serve llm`'s arguments for a model. `mmproj_path` and `draft_path` are ""
// when the model has none; `speculative` names the draft's mode (dflash2, mtp or
// dspark) and is ignored without a draft.
std::vector<std::string> build_server_argv(const std::string& model_name,
                                           const std::string& gguf_path,
                                           const std::string& mmproj_path,
                                           const std::string& draft_path,
                                           const std::string& speculative,
                                           int ctx_size,
                                           int port,
                                           const std::string& gufo_args);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<GufoServer>(); }
}  // namespace gufo

}  // namespace backends
}  // namespace lemon
