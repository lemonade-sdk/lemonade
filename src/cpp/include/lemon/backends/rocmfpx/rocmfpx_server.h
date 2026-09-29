#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"

#include <string>
#include <vector>

namespace lemon {
namespace backends {

// Runs the ROCm FPX llama-server fork from its container. The requests it
// serves are llama-server's, so it inherits them from LlamaCppServer and
// replaces only the launch.
class RocmFpxServer : public LlamaCppServer {
public:
    using LlamaCppServer::LlamaCppServer;
    ~RocmFpxServer() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;
};

namespace rocmfpx {
// llama-server's arguments for a model. `mmproj_path` and `draft_path` are ""
// when the model has none; `mtp` marks a draft as an MTP head.
std::vector<std::string> build_server_argv(const std::string& gguf_path,
                                           const std::string& mmproj_path,
                                           const std::string& draft_path,
                                           bool mtp,
                                           int ctx_size,
                                           int port,
                                           const std::string& rocmfpx_args);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() {
    return capability_mask_of<RocmFpxServer>() & ~(CAP_EMBEDDINGS | CAP_RERANKING);
}
}  // namespace rocmfpx

}  // namespace backends
}  // namespace lemon
