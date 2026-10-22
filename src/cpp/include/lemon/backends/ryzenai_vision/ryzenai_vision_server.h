#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server_capabilities.h"
#include "lemon/wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

// Runs an ONNX object detector on the Ryzen AI NPU as a ryzenai_vision_server.py
// subprocess, launched with the Ryzen AI Software environment's Python.
class RyzenAIVisionServer : public WrappedServer, public IDetectionServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    explicit RyzenAIVisionServer(const std::string& log_level,
                                 ModelManager* model_manager,
                                 BackendManager* backend_manager);

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade = false) override;

    json detect(const json& request) override;
};

namespace ryzenai_vision {
// The Ryzen AI Software Python interpreter, or "" with `error` explaining what
// is missing. Never throws: system-info calls it while listing backends.
std::string find_python(std::string& error);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<RyzenAIVisionServer>(); }
}  // namespace ryzenai_vision
}  // namespace backends
}  // namespace lemon
