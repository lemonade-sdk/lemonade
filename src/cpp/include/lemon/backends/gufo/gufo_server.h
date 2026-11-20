#pragma once

#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/wrapped_server.h"

#include <string>
#include <vector>

namespace lemon {
namespace backends {

class GufoServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    GufoServer(const std::string& log_level, ModelManager* model_manager,
               BackendManager* backend_manager);
    ~GufoServer() override;

    void load(const std::string& model_name, const ModelInfo& model_info,
              const RecipeOptions& options, bool do_not_upgrade = false) override;
    void unload() override;

    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;
};

namespace gufo {
// gufo takes each kind of draft through its own flag, and the draft's GGUF
// architecture says which kind it is. Throws for a draft gufo cannot run.
std::string speculative_mode(const std::string& draft_architecture,
                             const std::string& target_architecture);

// The gufo release a backend_versions.json pin names: the image's X.Y.Z tag is
// the release vX.Y.Z. A version that is not an image pin is already a release.
std::string release_tag(const std::string& version);

// The version a host install reports, given its version.txt, the
// backend_versions.json pin and the user's *_bin setting. An install of the
// pin's release reports the pin, which is what Lemonade expects; under an
// explicit release in *_bin it reports the release, which is then expected.
std::string installed_version(const std::string& file_version, const std::string& pin,
                              const std::string& user_pin);

// `gufo serve llm`'s arguments for a model. `mmproj_path` and `draft_path` are ""
// when the model has none; `speculative` is speculative_mode()'s answer for the
// draft and is ignored without one.
std::vector<std::string> build_server_argv(const std::string& model_name,
                                           const std::string& gguf_path,
                                           const std::string& mmproj_path,
                                           const std::string& draft_path,
                                           const std::string& speculative,
                                           int ctx_size,
                                           const std::string& host,
                                           int port,
                                           const std::string& gufo_args);

std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<GufoServer>(); }
}  // namespace gufo

}  // namespace backends
}  // namespace lemon
