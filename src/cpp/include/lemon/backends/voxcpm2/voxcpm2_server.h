#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/server_capabilities.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

class VoxCPM2Server : public WrappedServer, public ITextToSpeechServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    VoxCPM2Server(const std::string& log_level,
                  ModelManager* model_manager,
                  BackendManager* backend_manager);
    ~VoxCPM2Server() override;

    void load(const std::string& model_name,
              const ModelInfo& model_info,
              const RecipeOptions& options,
              bool do_not_upgrade) override;
    void unload() override;

    void audio_speech(const json& request, httplib::DataSink& sink) override;
    std::vector<std::string> supported_audio_formats() const override { return {"wav"}; }

    // The cpu variant runs entirely on the CPU, so the router must not account
    // for it against a GPU slot the way the descriptor's default_device implies.
    DeviceType effective_device(const RecipeOptions& options) const override;

private:
    // Resolves the backend variant the way load() will, or "" if this system
    // supports none. Kept in one place so the device reported before the load
    // cannot disagree with the variant the load actually picks.
    std::string resolve_backend(const RecipeOptions& options) const;
    std::string resolve_binary_path(const std::string& backend);
};

namespace voxcpm2 {
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace voxcpm2

}  // namespace backends
}  // namespace lemon
