#pragma once

#include "../wrapped_server.h"
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    FastFlowLMServer(const std::string& log_level = "info", ModelManager* model_manager = nullptr);

    ~FastFlowLMServer() override;

    void install(const std::string& backend = "") override;

    std::string download_model(const std::string& checkpoint,
                              bool do_not_upgrade = false);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // IEmbeddingsServer implementation
    json embeddings(const json& request) override;

    // IRerankingServer implementation
    json reranking(const json& request) override;

    // FLM uses /api/tags for readiness check instead of /health
    bool wait_for_ready();

    // Override to transform model name to checkpoint for FLM
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true) override;

private:
    // Existing methods
    std::string get_flm_path();
    bool check_npu_available();

    // Version management
    std::string get_flm_required_version();  // Get required version from backend_versions.json
    std::string get_flm_installed_version(); // Get currently installed version (empty if not installed)
    bool compare_versions(const std::string& v1, const std::string& v2); // true if v1 >= v2

    // NPU driver check
    std::string get_min_npu_driver_version();  // Get minimum driver version from backend_versions.json
    std::string get_npu_driver_version();      // Get current NPU driver version via WMI
    bool check_npu_driver_version();           // Check if NPU driver meets minimum requirements

    // Installation - returns true if FLM was upgraded (may invalidate existing models)
    bool install_flm_if_needed();
    bool download_flm_installer(const std::string& output_path);
    void run_flm_installer(const std::string& installer_path, bool silent);

    // Environment management
    void refresh_environment_path();
    bool verify_flm_installation(const std::string& expected_version, int max_retries = 10);

    // Cache for installed version (to avoid repeated calls to flm --version)
    mutable std::string cached_installed_version_;
    void invalidate_version_cache();  // Call after installation to force re-check

    // Track whether FLM was upgraded during install() - used to detect model invalidation
    bool flm_was_upgraded_ = false;

    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
