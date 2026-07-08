#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <filesystem>
#include <cstdint>
#include <string>

namespace lemon {
namespace backends {

std::map<std::string, nlohmann::json> parse_vllm_metrics_text(const std::string& body);

class VLLMServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);


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

    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

    std::map<std::string, nlohmann::json> get_additional_telemetry() override;
    std::string get_additional_telemetry_url() const override;
    std::function<std::map<std::string, nlohmann::json>(const std::string&)> get_additional_telemetry_parser() const override;

private:
    std::filesystem::path rocm_shim_dir_;

    json prepare_openai_request(const json& request);
    json fit_openai_max_tokens_to_context(const json& request);
    int64_t count_openai_prompt_tokens(const json& request);

    int64_t max_model_len_ = 0;
};

namespace vllm {
// Factory for the vllm backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
}  // namespace vllm
}  // namespace backends
}  // namespace lemon
