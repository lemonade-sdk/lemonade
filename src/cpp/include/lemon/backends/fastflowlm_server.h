#pragma once

#include "../wrapped_server.h"
#include "../model_manager.h"  // For DownloadProgressCallback
#include <string>

namespace lemon {
namespace backends {

class FastFlowLMServer : public WrappedServer, public IEmbeddingsServer, public IRerankingServer {
public:
    FastFlowLMServer(const std::string& log_level = "info", ModelManager* model_manager = nullptr,
                     BackendManager* backend_manager = nullptr);

    ~FastFlowLMServer() override;

    // Result of static install check
    struct InstallResult { bool was_upgraded; };

    // Static install entry point — no instance state needed.
    // Called by BackendManager::install_backend() and FastFlowLMServer::load().
    static InstallResult install_if_needed(DownloadProgressCallback progress_cb = nullptr);

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
    // Static helpers for install logic (no instance state needed)
    static std::string get_flm_path();
    static bool check_npu_available();

    // Version management
    static std::string get_flm_required_version();
    static std::string get_flm_installed_version();
    static bool compare_versions(const std::string& v1, const std::string& v2);

    // NPU driver check
    static std::string get_min_npu_driver_version();
    static std::string get_npu_driver_version();
    static bool check_npu_driver_version();

    // Installation - returns true if FLM was upgraded (may invalidate existing models)
    static bool install_flm_if_needed(DownloadProgressCallback progress_cb = nullptr);
    static bool download_flm_installer(const std::string& output_path,
                                       DownloadProgressCallback progress_cb = nullptr);
    static void run_flm_installer(const std::string& installer_path, bool silent);

    // Environment management
    static void refresh_environment_path();
    static bool verify_flm_installation(const std::string& expected_version, int max_retries = 10);

    // Cache management (function-local static in .cpp)
    static void invalidate_version_cache();

    bool is_loaded_ = false;
};

} // namespace backends
} // namespace lemon
