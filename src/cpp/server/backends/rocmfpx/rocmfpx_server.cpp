#include "lemon/backends/rocmfpx/rocmfpx_server.h"

#include <string>

#include <lemon/utils/aixlog.hpp>
#include "lemon/backends/rocmfpx/rocmfpx.h"
#include "lemon/model_manager.h"

namespace lemon {
namespace backends {

namespace {
constexpr const char* kBackend = "rocmfpx";
}  // namespace

RocmFpxServer::RocmFpxServer(const std::string& log_level, ModelManager* model_manager,
                             BackendManager* backend_manager)
    : LlamaCppServer(log_level, model_manager, backend_manager) {
    server_name_ = "rocmfpx";
}

RocmFpxServer::~RocmFpxServer() {
    unload();
}

LlamaCppServer::LlamaLaunch RocmFpxServer::launch_profile(const RecipeOptions& options) const {
    (void)options;  // this recipe has exactly one backend, so nothing to select
    const rocmfpx::LaunchDefaults tuned = rocmfpx::launch_defaults();

    LlamaLaunch launch;
    launch.recipe = rocmfpx::descriptor.recipe;
    launch.backend = kBackend;
    launch.args_option = "rocmfpx_args";
    launch.reserved_flags = &rocmfpx::reserved_custom_arg_flags();
    launch.batch_size = tuned.batch_size;
    launch.ubatch_size = tuned.ubatch_size;
    launch.flash_attention = tuned.flash_attention;
    launch.no_mmap = tuned.no_mmap;
    return launch;
}

namespace rocmfpx {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<RocmFpxServer>(ctx);
}

const BackendSpec* spec() {
    static const BackendSpec kSpec(descriptor.recipe, descriptor.binary, nullptr, false);
    return &kSpec;
}

const BackendOps* ops() {
    static const RocmFpxOps kOps(descriptor.recipe);
    return &kOps;
}

}  // namespace rocmfpx

}  // namespace backends
}  // namespace lemon
