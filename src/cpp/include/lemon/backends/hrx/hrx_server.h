#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"

#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

class HrxServer : public LlamaCppServer {
public:
    using LlamaCppServer::LlamaCppServer;

    static InstallParams get_install_params(const std::string& backend,
                                            const std::string& version);

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;
};

namespace hrx {
std::vector<std::string> build_server_argv(const std::string& gguf_path,
                                           int ctx_size,
                                           int port,
                                           const std::string& hrx_args);
std::vector<std::pair<std::string, std::string>> build_server_environment();

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() {
    return capability_mask_of<HrxServer>() &
           ~(CAP_EMBEDDINGS | CAP_RERANKING);
}
}  // namespace hrx

}  // namespace backends
}  // namespace lemon
