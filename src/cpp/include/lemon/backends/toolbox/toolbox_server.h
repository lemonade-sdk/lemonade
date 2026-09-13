#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/container_backend.h"
#include "lemon/backends/toolbox/toolbox.h"
#include "lemon/backends/llamacpp/llamacpp_ops.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"

#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

// Runs the prebuilt llama.cpp toolbox images as a Lemonade backend. The image
// puts llama-server on PATH and declares no entrypoint, so this is the ordinary
// llama.cpp server class with a containerized launch: everything above load() -
// chat, embeddings, reranking, slots, tokenize, streaming, downsize - is
// inherited unchanged.
class LlamaCppToolboxServer : public LlamaCppServer {
public:
    LlamaCppToolboxServer(const std::string& log_level, ModelManager* model_manager,
                          BackendManager* backend_manager);
    ~LlamaCppToolboxServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

private:
    std::string variant_;  // the image variant this instance is running
};

// The same llama.cpp model management as the llamacpp recipe, layered over the
// shared container install behavior.
class ToolboxOps : public LlamaCppOps<ContainerBackendOps> {
public:
    explicit ToolboxOps(std::string recipe)
        : LlamaCppOps<ContainerBackendOps>(std::move(recipe)) {}

    std::string args_option_name() const override { return "toolbox_args"; }

    // LlamaCppOps answers these for the PATH-installed "system" llama-server.
    // This recipe has no such variant: its version is the image digest and its
    // install check is whether that image is pulled.
    std::string resolve_version(const std::string& backend,
                                const std::string& file_version) const override {
        return ContainerBackendOps::resolve_version(backend, file_version);
    }
    InstallCheck check_install(const std::string& backend, bool binary_found) const override {
        return ContainerBackendOps::check_install(backend, binary_found);
    }
    std::string profile_id(const std::string& variant) const override {
        return toolbox::profile_for(variant);
    }
};

namespace toolbox {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<LlamaCppToolboxServer>(); }
}  // namespace toolbox

}  // namespace backends
}  // namespace lemon
