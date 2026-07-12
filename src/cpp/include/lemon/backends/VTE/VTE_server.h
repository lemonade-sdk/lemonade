#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// Wraps a single vte-server subprocess (one VTEModel per process, exactly
// like every other backend here -- see lemon::backends::VTE::descriptor for
// why this integration lives under a differently-cased folder). Only
// ICompletionServer is implemented for v1: VTE has no embeddings/reranking/
// tokenizer capability today.
class VTEServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    VTEServer(const std::string& log_level, ModelManager* model_manager, BackendManager* backend_manager);

    ~VTEServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;

private:
    int context_length_ = 0;
};

namespace VTE {
// Factory for the vte backend (constructs the server class -- lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace VTE
}  // namespace backends
}  // namespace lemon
