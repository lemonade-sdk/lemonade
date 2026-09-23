#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/container_backend.h"
#include "lemon/backends/llamacpp/llamacpp_ops.h"
#include "lemon/backends/llamacpp/llamacpp_server.h"
#include "lemon/backends/rocmfpx/rocmfpx.h"

#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

// Runs the prebuilt ROCm FPX image. The image puts llama-server on PATH and
// declares no entrypoint, so this fills in a LlamaLaunch and inherits the rest
// of LlamaCppServer unchanged.
class RocmFpxServer : public LlamaCppServer {
public:
    RocmFpxServer(const std::string& log_level, ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~RocmFpxServer() override;

protected:
    LlamaLaunch launch_profile(const RecipeOptions& options) const override;
};

// The same llama.cpp model management as the llamacpp recipe, layered over the
// shared container install behavior.
class RocmFpxOps : public LlamaCppOps<ContainerBackendOps> {
public:
    explicit RocmFpxOps(std::string recipe)
        : LlamaCppOps<ContainerBackendOps>(std::move(recipe)) {}

    std::string args_option_name() const override { return "rocmfpx_args"; }

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
};

namespace rocmfpx {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RocmFpxServer>(); }
}  // namespace rocmfpx

}  // namespace backends
}  // namespace lemon
