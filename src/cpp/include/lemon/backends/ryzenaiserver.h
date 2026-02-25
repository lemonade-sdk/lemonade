#pragma once

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"
#include <string>

namespace lemon {

using backends::BackendSpec;
using backends::InstallParams;

class RyzenAIServer : public WrappedServer {
public:
#ifndef LEMONADE_TRAY
    static InstallParams get_install_params(const std::string& backend, const std::string& version);
#endif

    inline static const BackendSpec SPEC = BackendSpec(
        "ryzenai-server",
#ifdef _WIN32
        "ryzenai-server.exe"
#else
        "ryzenai-server"
#endif
#ifndef LEMONADE_TRAY
        , get_install_params
#endif
    );

    RyzenAIServer(const std::string& model_name, bool debug, ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~RyzenAIServer() override;

    // Installation and availability
    static bool is_available();

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    // RyzenAI-specific: set model path before loading
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
