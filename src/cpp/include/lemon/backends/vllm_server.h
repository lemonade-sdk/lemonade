#pragma once

#include "../wrapped_server.h"
#include "backend_utils.h"
#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

class VLLMServer : public WrappedServer,
                   public IAnthropicServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    inline static const BackendSpec SPEC = BackendSpec(
            "vllm",
            "vllm-server"
        , get_install_params
        , /*supports_split_archive=*/true
    );

    VLLMServer(const std::string& log_level,
               ModelManager* model_manager,
               BackendManager* backend_manager);

    ~VLLMServer() override;

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IAnthropicServer implementation
    json anthropic_messages(const json& request) override;
    void anthropic_messages_stream(const std::string& request_body, httplib::DataSink& sink) override;
    json anthropic_count_tokens(const json& request) override;

private:
    json fit_anthropic_max_tokens_to_context(const json& request);

    int64_t max_model_len_ = 0;
};

} // namespace backends
} // namespace lemon
