#pragma once

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"
#include <string>

namespace lemon {

using backends::BackendSpec;
using backends::InstallParams;

// Wraps the standalone amdgpu-server (OGA + hipep/AMDGPU EP) subprocess that
// runs ONNX GenAI models on the AMD iGPU (gfx1151). Mirrors RyzenAIServer; the
// only material differences are the binary, the "gpu" device variant, and that
// the model runs on the GPU rather than the NPU.
class AMDGPUServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
        "amdgpu-server",
#ifdef _WIN32
        "amdgpu-server.exe"
#else
        "amdgpu-server"
#endif
        , get_install_params
    );

    AMDGPUServer(const std::string& model_name, bool debug, ModelManager* model_manager,
                 BackendManager* backend_manager);
    ~AMDGPUServer() override;

    // Installation and availability
    static bool is_available();

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // Set model path before loading
    void set_model_path(const std::string& path) { model_path_ = path; }

    void unload() override;

    // Inference operations (from ICompletionServer via WrappedServer)
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

private:
    std::string model_name_;
    std::string model_path_;
    bool is_loaded_;
};

} // namespace lemon
